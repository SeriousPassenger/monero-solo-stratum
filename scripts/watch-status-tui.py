#!/usr/bin/env python3
"""Unified split-screen monitor for monero-solo-stratum.

The full-screen UI uses only the Python standard library.  One background
worker gathers pool, monerod and host metrics while another follows the JSONL
debug log.  The curses thread never performs network or file I/O.

Python 3.10+.
"""

from __future__ import annotations

import argparse
import collections
import curses
import dataclasses
import datetime as _datetime
import errno
import heapq
import http.client
import json
import locale
import math
import os
import queue
import random
import signal
import socket
import stat
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any, BinaryIO, Deque, Dict, Iterable, List, Mapping, MutableMapping, Optional, Sequence, Set, Tuple


DEFAULT_API_URL = "http://127.0.0.1:8787"
DEFAULT_MONERO_RPC_URL = "http://127.0.0.1:18081"
DEFAULT_EVENT_LOG = "/var/lib/monero-solo-stratum/debug.jsonl"
DEFAULT_EVENT_RATE = 2.0
DEFAULT_MIN_SHARE_DIFFICULTY = 100_000_000
DEFAULT_HISTORY_SIZE = 2_000
MAX_HISTORY_SIZE = 100_000
MAX_SELECTED = 10_000
MAX_EXPORT_BYTES = 256 * 1024 * 1024
MAX_EVENT_LINE = 1 << 20
READ_CHUNK = 1 << 16
SAMPLING_WINDOW = 1.0


FILTER_GROUPS: Tuple[Tuple[str, str], ...] = (
    ("high-shares", "Accepted / high shares"),
    ("exceptional-shares", "Exceptional shares"),
    ("templates", "Templates"),
    ("jobs", "Jobs"),
    ("connections", "Connections"),
    ("candidates", "Candidates / blocknotify"),
    ("system", "Runtime / node / verifier"),
    ("misc", "Warnings / miscellaneous"),
)
FILTER_KEYS = tuple(key for key, _label in FILTER_GROUPS)


@dataclasses.dataclass(frozen=True)
class EventEntry:
    sequence: int
    event: Dict[str, Any]

    @property
    def raw(self) -> Dict[str, Any]:
        return self.event


def event_dict(value: Any) -> Dict[str, Any]:
    if isinstance(value, EventEntry):
        return value.event
    if isinstance(value, dict):
        return value
    for attribute in ("event", "raw", "data"):
        candidate = getattr(value, attribute, None)
        if isinstance(candidate, dict):
            return candidate
    raise TypeError("event must be a mapping or EventEntry")


def event_fields(value: Any) -> Dict[str, Any]:
    fields = event_dict(value).get("fields")
    return fields if isinstance(fields, dict) else {}


def nonnegative_int(value: Any) -> Optional[int]:
    if value is None or isinstance(value, bool):
        return None
    try:
        result = int(str(value), 10)
    except (TypeError, ValueError):
        return None
    return result if result >= 0 else None


def finite_float(value: Any) -> Optional[float]:
    if value is None or isinstance(value, bool):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def deep_prune_empty(value: Any) -> Any:
    """Remove null and structurally empty values while retaining 0/false."""
    if isinstance(value, dict):
        result: Dict[str, Any] = {}
        for key, item in value.items():
            cleaned = deep_prune_empty(item)
            if cleaned is None or cleaned == "" or cleaned == [] or cleaned == {}:
                continue
            result[str(key)] = cleaned
        return result
    if isinstance(value, (list, tuple)):
        result_list = []
        for item in value:
            cleaned = deep_prune_empty(item)
            if cleaned is None or cleaned == "" or cleaned == [] or cleaned == {}:
                continue
            result_list.append(cleaned)
        return result_list
    return value


def export_selected_events(path: Path, records: Iterable[Any],
                           cancel: Optional[threading.Event] = None) -> None:
    """Atomically write selected raw records as a mode-0600 JSON array."""
    destination = Path(path).expanduser()
    if not destination.is_absolute():
        destination = Path.cwd() / destination
    if destination.is_symlink():
        raise ValueError("export destination must not be a symbolic link")
    if destination.exists() and not destination.is_file():
        raise ValueError("export destination must be a regular file")
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=str(destination.parent)
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            descriptor = -1
            stream.write("[\n")
            written = 2
            first = True
            for record in records:
                if cancel is not None and cancel.is_set():
                    raise InterruptedError("export cancelled")
                serialized = json.dumps(
                    deep_prune_empty(event_dict(record)),
                    ensure_ascii=False,
                    indent=2,
                )
                encoded_size = len(serialized.encode("utf-8")) + (0 if first else 2)
                written += encoded_size
                if written > MAX_EXPORT_BYTES:
                    raise ValueError(
                        f"export exceeds the {human_bytes(MAX_EXPORT_BYTES)} safety limit"
                    )
                if not first:
                    stream.write(",\n")
                stream.write(serialized)
                first = False
            stream.write("\n]\n")
            stream.flush()
            os.fsync(stream.fileno())
        if destination.is_symlink():
            raise ValueError("export destination became a symbolic link")
        os.replace(temporary, destination)
        os.chmod(destination, 0o600)
        try:
            directory_descriptor = os.open(destination.parent, os.O_RDONLY)
        except OSError:
            directory_descriptor = -1
        if directory_descriptor >= 0:
            try:
                os.fsync(directory_descriptor)
            finally:
                os.close(directory_descriptor)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


@dataclasses.dataclass(frozen=True)
class ExportResult:
    path: Path
    count: int
    error: Optional[str] = None


class ExportWorker:
    """Run potentially slow JSON/fsync work away from the curses thread."""

    def __init__(self) -> None:
        self.tasks: "queue.Queue[Optional[Tuple[Path, Tuple[EventEntry, ...]]]]" = queue.Queue(maxsize=1)
        self.results: "queue.Queue[ExportResult]" = queue.Queue(maxsize=1)
        self.cancel = threading.Event()
        self.thread = threading.Thread(target=self._run, name="mss-export", daemon=False)
        self.thread.start()

    def submit(self, path: Path, records: Sequence[EventEntry]) -> bool:
        try:
            self.tasks.put_nowait((Path(path), tuple(records)))
            return True
        except queue.Full:
            return False

    def _run(self) -> None:
        while not self.cancel.is_set():
            try:
                task = self.tasks.get(timeout=0.2)
            except queue.Empty:
                continue
            if task is None:
                return
            path, records = task
            error: Optional[str] = None
            try:
                export_selected_events(path, records, self.cancel)
            except Exception as exception:
                error = str(exception)
            replace_latest(self.results, ExportResult(path, len(records), error))

    def close(self) -> None:
        self.cancel.set()
        try:
            self.tasks.put_nowait(None)
        except queue.Full:
            pass
        self.thread.join()


@dataclasses.dataclass(frozen=True)
class Theme:
    name: str
    rich: bool
    colors: Mapping[str, Tuple[int, int]]
    border: Tuple[str, str, str, str, str, str]
    cursor_marker: str
    selected_marker: str


# Curses uses the terminal palette rather than RGB.  Values are xterm-256
# indexes; select_theme safely degrades them to ANSI-8 or monochrome roles.
THEMES: Dict[str, Theme] = {
    "nerv-asuka": Theme(
        "nerv-asuka", True,
        {"body": (230, 234), "panel": (230, 235), "border": (160, 234),
         "focus": (196, 234), "accent": (202, 234), "ok": (148, 234),
         "warn": (214, 234), "error": (203, 234), "dim": (245, 234),
         "select": (234, 202), "title": (230, 160)},
        ("┏", "┓", "┗", "┛", "━", "┃"), "›", "✓"),
    "tokyo-neon": Theme(
        "tokyo-neon", True,
        {"body": (255, 232), "panel": (255, 233), "border": (61, 232),
         "focus": (51, 232), "accent": (201, 232), "ok": (85, 232),
         "warn": (221, 232), "error": (205, 232), "dim": (103, 232),
         "select": (255, 54), "title": (232, 51)},
        ("╭", "╮", "╰", "╯", "─", "│"), "›", "✓"),
    "windows-classic": Theme(
        "windows-classic", True,
        {"body": (16, 30), "panel": (16, 250), "border": (16, 250),
         "focus": (231, 19), "accent": (19, 250), "ok": (28, 250),
         "warn": (100, 250), "error": (160, 250), "dim": (240, 250),
         "select": (231, 19), "title": (231, 19)},
        ("┌", "┐", "└", "┘", "─", "│"), "›", "✓"),
    "black": Theme(
        "black", False,
        {"body": (7, 0), "panel": (7, 0), "border": (7, 0),
         "focus": (6, 0), "accent": (5, 0), "ok": (2, 0),
         "warn": (3, 0), "error": (1, 0), "dim": (7, 0),
         "select": (0, 7), "title": (0, 6)},
        ("+", "+", "+", "+", "-", "|"), ">", "*"),
    "windows-classic-tty": Theme(
        "windows-classic-tty", False,
        {"body": (7, 0), "panel": (0, 7), "border": (0, 7),
         "focus": (7, 4), "accent": (4, 7), "ok": (2, 7),
         "warn": (3, 7), "error": (1, 7), "dim": (0, 7),
         "select": (7, 4), "title": (7, 4)},
        ("+", "+", "+", "+", "-", "|"), ">", "*"),
}


ANSI8_MAP = {16: 0, 19: 4, 28: 2, 30: 6, 51: 6, 54: 4, 61: 4,
             85: 2, 100: 3, 103: 7, 148: 2, 160: 1, 196: 1,
             201: 5, 202: 3, 203: 1, 205: 5, 214: 3, 221: 3,
             230: 7, 231: 7, 232: 0, 233: 0, 234: 0, 235: 0,
             240: 0, 245: 7, 250: 7, 255: 7}


def select_theme(name: str, colors: int) -> Theme:
    if name not in THEMES:
        raise ValueError(f"unknown theme: {name}")
    theme = THEMES[name]
    if colors >= 256:
        return theme
    if colors >= 8:
        mapped = {role: (ANSI8_MAP.get(fg, fg % 8), ANSI8_MAP.get(bg, bg % 8))
                  for role, (fg, bg) in theme.colors.items()}
        return dataclasses.replace(theme, rich=False, colors=mapped,
                                   border=THEMES["black"].border,
                                   cursor_marker=">", selected_marker="*")
    monochrome = {role: (-1, -1) for role in theme.colors}
    return dataclasses.replace(theme, rich=False, colors=monochrome,
                               border=THEMES["black"].border,
                               cursor_marker=">", selected_marker="*")


@dataclasses.dataclass(frozen=True)
class Rect:
    y: int
    x: int
    height: int
    width: int


@dataclasses.dataclass(frozen=True)
class LayoutResult:
    status: Optional[Rect]
    events: Optional[Rect]
    orientation: str
    compact: bool


def resolve_layout(view: str, layout: str, reverse: bool,
                   rows: int, columns: int) -> LayoutResult:
    rows = max(0, rows)
    columns = max(0, columns)
    usable_y = 1 if rows > 2 else 0
    usable_height = max(0, rows - (2 if rows > 2 else 0))
    if view == "status":
        return LayoutResult(Rect(usable_y, 0, usable_height, columns), None,
                            "single", rows < 12 or columns < 45)
    if view in {"events", "stream"}:
        return LayoutResult(None, Rect(usable_y, 0, usable_height, columns),
                            "single", rows < 12 or columns < 45)
    orientation = layout
    if orientation == "auto":
        orientation = "horizontal" if columns >= 105 else "vertical"
    if orientation == "horizontal":
        first_width = max(0, int(columns * 0.46))
        second_width = max(0, columns - first_width)
        first = Rect(usable_y, 0, usable_height, first_width)
        second = Rect(usable_y, first_width, usable_height, second_width)
    else:
        first_height = max(0, usable_height // 2)
        second_height = max(0, usable_height - first_height)
        first = Rect(usable_y, 0, first_height, columns)
        second = Rect(usable_y + first_height, 0, second_height, columns)
    status, events = (second, first) if reverse else (first, second)
    return LayoutResult(status, events, orientation,
                        rows < 18 or columns < 60)


def event_group(value: Any) -> str:
    event = event_dict(value)
    code = str(event.get("code", ""))
    severity = str(event.get("severity", "")).lower()
    fields = event_fields(event)
    if code == "share.completed":
        return ("high-shares" if str(fields.get("status", "")) == "accepted"
                else "exceptional-shares")
    if code == "share.received":
        return "high-shares"
    if code.startswith("template."):
        return "templates"
    if code.startswith("job."):
        return "jobs"
    if code.startswith("connection."):
        return "connections"
    if code.startswith("candidate.") or code.startswith("blocknotify."):
        return "candidates"
    if code.startswith(("runtime.", "daemon.", "zmq.", "verifier.",
                        "database.", "entropy.")):
        return "system"
    if severity in {"warning", "warn", "error", "fatal", "critical"}:
        return "misc"
    return "misc"


FILTER_ALIASES: Dict[str, Set[str]] = {
    "all": set(FILTER_KEYS),
    "share": {"high-shares", "exceptional-shares"},
    "shares": {"high-shares", "exceptional-shares"},
    "high": {"high-shares"},
    "accepted": {"high-shares"},
    "exceptional": {"exceptional-shares"},
    "template": {"templates"},
    "templates": {"templates"},
    "job": {"jobs"},
    "jobs": {"jobs"},
    "connection": {"connections"},
    "connections": {"connections"},
    "candidate": {"candidates"},
    "candidates": {"candidates"},
    "blocknotify": {"candidates"},
    "runtime": {"system"},
    "system": {"system"},
    "node": {"system"},
    "verifier": {"system"},
    "warning": {"misc"},
    "warnings": {"misc"},
    "misc": {"misc"},
}


class EventFilter:
    def __init__(self, specification: Any = "") -> None:
        explicit_collection = isinstance(specification, (set, frozenset, list, tuple))
        if explicit_collection:
            requested = {str(item) for item in specification}
        else:
            text = str(specification or "").strip()
            requested = {part.strip().lower() for part in text.split(",") if part.strip()}
        if not requested:
            # An omitted/empty CLI value means the friendly default of all
            # categories.  An explicitly empty checkbox collection means none.
            self.enabled = set() if explicit_collection else set(FILTER_KEYS)
            return
        enabled: Set[str] = set()
        for item in requested:
            if item in FILTER_KEYS:
                enabled.add(item)
            elif item in FILTER_ALIASES:
                enabled.update(FILTER_ALIASES[item])
            else:
                raise ValueError(f"unknown stream category: {item}")
        self.enabled = enabled

    def matches(self, value: Any) -> bool:
        return event_group(value) in self.enabled

    match = matches
    allows = matches
    accepts = matches

    def __call__(self, value: Any) -> bool:
        return self.matches(value)

    def as_specification(self) -> str:
        return ",".join(key for key in FILTER_KEYS if key in self.enabled)


class CategoryChecklist:
    """Pure model for the multi-select stream-filter dialog."""

    def __init__(self, enabled: Iterable[str]) -> None:
        self.original = set(enabled)
        self.enabled = set(enabled)
        self.cursor = 0

    def handle(self, key: Any) -> Optional[str]:
        if key in (curses.KEY_UP, "k"):
            self.cursor = (self.cursor - 1) % len(FILTER_GROUPS)
        elif key in (curses.KEY_DOWN, "j"):
            self.cursor = (self.cursor + 1) % len(FILTER_GROUPS)
        elif key in (" ", ord(" ")):
            category = FILTER_GROUPS[self.cursor][0]
            if category in self.enabled:
                self.enabled.remove(category)
            else:
                self.enabled.add(category)
        elif key in ("a", "A", ord("a"), ord("A")):
            self.enabled = set(FILTER_KEYS)
        elif key in ("n", "N", ord("n"), ord("N")):
            self.enabled.clear()
        elif key in (10, 13, curses.KEY_ENTER, "\n"):
            return "apply"
        elif key in (27, "\x1b"):
            self.enabled = set(self.original)
            return "cancel"
        return None


class EventBuffer:
    def __init__(self, capacity: int) -> None:
        if capacity <= 0:
            raise ValueError("history capacity must be positive")
        self.capacity = capacity
        self._items: Deque[EventEntry] = collections.deque()
        self._selected: Set[int] = set()
        self.cursor_sequence: Optional[int] = None
        self.follow = True

    def __len__(self) -> int:
        return len(self._items)

    def __iter__(self):
        return iter(self._items)

    def append(self, item: EventEntry) -> None:
        while len(self._items) >= self.capacity:
            removed = self._items.popleft()
            self._selected.discard(removed.sequence)
        self._items.append(item)
        if self.follow or self.cursor_sequence is None:
            self.cursor_sequence = item.sequence

    def _index(self) -> Optional[int]:
        if not self._items:
            return None
        for index, item in enumerate(self._items):
            if item.sequence == self.cursor_sequence:
                return index
        self.cursor_sequence = self._items[0].sequence
        return 0

    def _matching(self, event_filter: Optional[EventFilter]) -> List[EventEntry]:
        return [item for item in self._items
                if event_filter is None or event_filter.matches(item)]

    def ensure_visible(self, event_filter: Optional[EventFilter]) -> Optional[EventEntry]:
        candidates = self._matching(event_filter)
        if not candidates:
            return None
        was_following = self.follow
        current = next((item for item in candidates
                        if item.sequence == self.cursor_sequence), None)
        if current is None:
            current = candidates[-1]
            self.cursor_sequence = current.sequence
            self.follow = was_following
        return current

    def current(self, event_filter: Optional[EventFilter] = None) -> Optional[EventEntry]:
        if event_filter is not None:
            return self.ensure_visible(event_filter)
        index = self._index()
        return None if index is None else list(self._items)[index]

    def move(self, delta: int, event_filter: Optional[EventFilter] = None) -> None:
        candidates = self._matching(event_filter)
        if not candidates:
            return
        current = self.ensure_visible(event_filter)
        index = next((position for position, item in enumerate(candidates)
                      if current is not None and item.sequence == current.sequence), 0)
        index = max(0, min(len(candidates) - 1, index + delta))
        self.cursor_sequence = candidates[index].sequence
        self.follow = index == len(candidates) - 1

    def home(self, event_filter: Optional[EventFilter] = None) -> None:
        candidates = self._matching(event_filter)
        if candidates:
            self.cursor_sequence = candidates[0].sequence
            self.follow = False

    def end(self, event_filter: Optional[EventFilter] = None) -> None:
        candidates = self._matching(event_filter)
        if candidates:
            self.cursor_sequence = candidates[-1].sequence
            self.follow = True

    def toggle(self, event_filter: Optional[EventFilter] = None) -> bool:
        current = self.current(event_filter)
        if current is None:
            return False
        sequence = current.sequence
        if sequence in self._selected:
            self._selected.remove(sequence)
            return False
        if len(self._selected) >= MAX_SELECTED:
            raise ValueError(f"selection is limited to {MAX_SELECTED} records")
        self._selected.add(sequence)
        return True

    def clear_selection(self) -> None:
        self._selected.clear()

    def select_sequences(self, sequences: Iterable[int]) -> None:
        valid = {item.sequence for item in self._items}
        for sequence in sequences:
            if sequence in valid and len(self._selected) < MAX_SELECTED:
                self._selected.add(sequence)

    def is_selected(self, sequence: int) -> bool:
        return sequence in self._selected

    def selected_events(self) -> List[EventEntry]:
        return [item for item in self._items if item.sequence in self._selected]

    def visible(self, height: int, event_filter: Optional[EventFilter] = None) -> List[EventEntry]:
        if height <= 0:
            return []
        candidates = self._matching(event_filter)
        if not candidates:
            return []
        self.ensure_visible(event_filter)
        cursor = next((index for index, item in enumerate(candidates)
                       if item.sequence == self.cursor_sequence), len(candidates) - 1)
        start = max(0, cursor - height + 1)
        return candidates[start:start + height]


class Reservoir:
    def __init__(self, capacity: int, rng: random.Random) -> None:
        self.capacity = capacity
        self.rng = rng
        self.seen = 0
        self.items: List[EventEntry] = []

    def add(self, item: EventEntry) -> None:
        self.seen += 1
        if len(self.items) < self.capacity:
            self.items.append(item)
            return
        position = self.rng.randrange(self.seen)
        if position < self.capacity:
            self.items[position] = item

    def clear(self) -> None:
        self.seen = 0
        self.items.clear()

    def __len__(self) -> int:
        return len(self.items)


SAMPLER_CLASSES = ("share", "template", "job", "connection", "misc")
SAMPLER_CYCLE = ("share", "share", "template", "job", "connection", "misc")


class AdaptiveSampler:
    """One global row budget with top shares and random non-share sampling."""

    def __init__(self, rate: Optional[float] = None,
                 min_share_difficulty: int = DEFAULT_MIN_SHARE_DIFFICULTY,
                 rng: Optional[random.Random] = None,
                 rows_per_second: Optional[float] = None) -> None:
        if rate is None:
            rate = rows_per_second
        if rate is None or not math.isfinite(float(rate)) or float(rate) <= 0:
            raise ValueError("event rate must be positive")
        self.min_share_difficulty = int(min_share_difficulty)
        self.rng = rng or random.SystemRandom()
        self._rate = float(rate)
        self.capacity = self._capacity_for_rate(self._rate)
        self.share_heap: List[Tuple[int, int, EventEntry]] = []
        self.reservoirs = {
            category: Reservoir(self.capacity, self.rng)
            for category in SAMPLER_CLASSES if category != "share"
        }
        self.carry = 0.0
        self.priority_debt = 0
        self.cycle_cursor = 0

    @staticmethod
    def _capacity_for_rate(rate: float) -> int:
        return min(4096, max(8, int(math.ceil(rate * 2)) + 4))

    @property
    def rate(self) -> float:
        return self._rate

    @rate.setter
    def rate(self, value: float) -> None:
        rate = float(value)
        if not math.isfinite(rate) or rate <= 0:
            raise ValueError("event rate must be positive")
        self._rate = rate
        new_capacity = self._capacity_for_rate(rate)
        if new_capacity == getattr(self, "capacity", new_capacity):
            self.capacity = new_capacity
            return
        self.capacity = new_capacity
        if hasattr(self, "share_heap") and len(self.share_heap) > new_capacity:
            self.share_heap = heapq.nlargest(new_capacity, self.share_heap)
            heapq.heapify(self.share_heap)
        for reservoir in getattr(self, "reservoirs", {}).values():
            reservoir.capacity = new_capacity
            if len(reservoir.items) > new_capacity:
                reservoir.items = self.rng.sample(reservoir.items, new_capacity)

    @staticmethod
    def _class(value: Any) -> str:
        code = str(event_dict(value).get("code", ""))
        if code == "share.completed":
            return "share"
        if code.startswith("template."):
            return "template"
        if code.startswith("job."):
            return "job"
        if code.startswith("connection."):
            return "connection"
        return "misc"

    @staticmethod
    def is_priority(value: Any) -> bool:
        event = event_dict(value)
        code = str(event.get("code", ""))
        severity = str(event.get("severity", "")).lower()
        fields = event_fields(event)
        if severity in {"warning", "warn", "error", "fatal", "critical"}:
            return True
        if code.startswith(("candidate.", "blocknotify.", "runtime.",
                            "daemon.", "zmq.", "verifier.", "database.",
                            "entropy.")):
            return True
        return code == "share.completed" and str(fields.get("status", "")) != "accepted"

    def add(self, item: EventEntry) -> str:
        event = event_dict(item)
        code = str(event.get("code", ""))
        if code == "share.received":
            return "drop"
        if self.is_priority(item):
            self.priority_debt += 1
            return "priority"
        category = self._class(item)
        if category == "share":
            difficulty = nonnegative_int(event_fields(event).get("actual_difficulty")) or 0
            if difficulty < self.min_share_difficulty:
                return "drop"
            candidate = (difficulty, item.sequence, item)
            if len(self.share_heap) < self.capacity:
                heapq.heappush(self.share_heap, candidate)
            elif candidate[:2] > self.share_heap[0][:2]:
                heapq.heapreplace(self.share_heap, candidate)
        else:
            self.reservoirs[category].add(item)
        return "sampled"

    def _counts(self) -> Dict[str, int]:
        return {"share": len(self.share_heap),
                **{category: len(value) for category, value in self.reservoirs.items()}}

    def _allocate(self, slots: int) -> Dict[str, int]:
        available = self._counts()
        result = {category: 0 for category in SAMPLER_CLASSES}
        while slots > 0 and any(available.values()):
            chosen = None
            for _ in range(len(SAMPLER_CYCLE)):
                category = SAMPLER_CYCLE[self.cycle_cursor]
                self.cycle_cursor = (self.cycle_cursor + 1) % len(SAMPLER_CYCLE)
                if available.get(category, 0) > 0:
                    chosen = category
                    break
            if chosen is None:
                break
            result[chosen] += 1
            available[chosen] -= 1
            slots -= 1
        return result

    def flush(self, elapsed: float = SAMPLING_WINDOW) -> List[EventEntry]:
        elapsed = max(0.0, min(float(elapsed), SAMPLING_WINDOW * 1.25))
        self.carry += self.rate * elapsed
        earned = int(math.floor(self.carry + 1e-12))
        self.carry -= earned
        if earned <= 0:
            return []
        slots = max(0, earned - self.priority_debt)
        self.priority_debt = max(0, self.priority_debt - earned)
        allocation = self._allocate(slots)
        selected: List[EventEntry] = []
        if allocation["share"]:
            ranked = sorted(self.share_heap, reverse=True)
            selected.extend(item[2] for item in ranked[:allocation["share"]])
        for category, reservoir in self.reservoirs.items():
            count = allocation[category]
            if count >= len(reservoir.items):
                selected.extend(reservoir.items)
            elif count:
                selected.extend(self.rng.sample(reservoir.items, count))
        selected.sort(key=lambda item: item.sequence)
        self.share_heap.clear()
        for reservoir in self.reservoirs.values():
            reservoir.clear()
        return selected


class JsonlFollower:
    """Nonblocking tail -F reader with partial-line and copytruncate support."""

    def __init__(self, path: Path, from_start: bool = False) -> None:
        self.path = Path(path)
        self.from_start = from_start
        self.handle: Optional[BinaryIO] = None
        self.identity: Optional[Tuple[int, int]] = None
        self.buffer = bytearray()
        self.first_open = True
        self.discard_long = False

    def open(self) -> bool:
        try:
            handle = self.path.open("rb", buffering=0)
        except OSError:
            return False
        details = os.fstat(handle.fileno())
        self.handle = handle
        self.identity = (details.st_dev, details.st_ino)
        if self.first_open and not self.from_start:
            handle.seek(0, os.SEEK_END)
        self.first_open = False
        return True

    def close(self) -> None:
        if self.handle is not None:
            self.handle.close()
            self.handle = None

    def _extract(self, maximum: int = 2048) -> List[bytes]:
        records: List[bytes] = []
        while len(records) < maximum:
            newline = self.buffer.find(b"\n")
            if newline < 0:
                if len(self.buffer) > MAX_EVENT_LINE:
                    self.buffer.clear()
                    self.discard_long = True
                break
            line = bytes(self.buffer[:newline])
            del self.buffer[:newline + 1]
            if self.discard_long:
                self.discard_long = False
                continue
            if len(line) <= MAX_EVENT_LINE:
                records.append(line)
        return records

    def poll(self) -> List[bytes]:
        if self.handle is None and not self.open():
            return []
        assert self.handle is not None
        if b"\n" in self.buffer:
            return self._extract()
        try:
            chunk = os.read(self.handle.fileno(), READ_CHUNK)
        except OSError:
            self.close()
            return []
        if chunk:
            self.buffer.extend(chunk)
            return self._extract()
        try:
            details = self.path.stat()
            position = self.handle.tell()
        except FileNotFoundError:
            return []
        except OSError:
            return []
        identity = (details.st_dev, details.st_ino)
        if identity != self.identity:
            self.close()
            self.buffer.clear()
            self.discard_long = False
            self.open()
        elif details.st_size < position:
            self.handle.seek(0)
            self.buffer.clear()
            self.discard_long = False
        return []


def parse_json_event(raw: bytes) -> Optional[Dict[str, Any]]:
    if not raw.strip():
        return None
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None
    return value if isinstance(value, dict) else None


def parse_proc_stat(text: str) -> Tuple[int, int]:
    for line in text.splitlines():
        fields = line.split()
        if fields and fields[0] == "cpu" and len(fields) >= 5:
            values = [int(value) for value in fields[1:]]
            total = sum(values)
            idle = values[3] + (values[4] if len(values) > 4 else 0)
            return total, idle
    raise ValueError("aggregate cpu row not found")


def parse_meminfo(text: str) -> Tuple[int, int]:
    values: Dict[str, int] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        name, remainder = line.split(":", 1)
        parts = remainder.split()
        if not parts:
            continue
        try:
            number = int(parts[0])
        except ValueError:
            continue
        multiplier = 1024 if len(parts) > 1 and parts[1].lower() == "kb" else 1
        values[name] = number * multiplier
    total = values.get("MemTotal", 0)
    available = values.get("MemAvailable", values.get("MemFree", 0))
    return total, available


def parse_net_dev(text: str) -> Tuple[int, int]:
    received = 0
    transmitted = 0
    for line in text.splitlines():
        if ":" not in line:
            continue
        interface, raw_values = line.split(":", 1)
        interface = interface.strip()
        fields = raw_values.split()
        if interface == "lo" or len(fields) < 9:
            continue
        try:
            received += int(fields[0])
            transmitted += int(fields[8])
        except ValueError:
            continue
    return received, transmitted


def parse_diskstats(text: str,
                    device: Optional[Tuple[int, int]] = None) -> Tuple[int, int, int]:
    """Return sectors read, sectors written, and aggregate busy milliseconds."""
    parsed: List[Tuple[int, int, str, int, int, int]] = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 14:
            continue
        try:
            parsed.append((int(fields[0]), int(fields[1]), fields[2],
                           int(fields[5]), int(fields[9]), int(fields[12])))
        except ValueError:
            continue
    if device is not None:
        exact = [row for row in parsed if row[:2] == device]
        if exact:
            return exact[0][3], exact[0][4], exact[0][5]

    # When a software aggregate is present, report that layer instead of both
    # it and its backing physical disks. This avoids double-counting md0 I/O on
    # the deployment's RAID0-over-NVMe layout. Exact device matching above is
    # preferred whenever the filesystem device can be resolved.
    aggregate_rows = [row for row in parsed
                      if row[2].startswith("md") or row[2].startswith("dm-")]
    candidates = aggregate_rows if aggregate_rows else parsed
    sectors_read = 0
    sectors_written = 0
    busy_ms = 0
    for _major, _minor, name, read, written, busy in candidates:
        # Whole disks only.  md/dm devices are admitted; common partitions are
        # excluded to avoid double counting their parent disk.
        if (name.startswith(("loop", "ram", "zram")) or
                (name.startswith("nvme") and "p" in name) or
                (name.startswith(("sd", "vd", "xvd")) and name[-1:].isdigit())):
            continue
        sectors_read += read
        sectors_written += written
        busy_ms += busy
    return sectors_read, sectors_written, busy_ms


def read_text(path: str) -> str:
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


@dataclasses.dataclass
class HostSample:
    captured: float
    cpu_total: int
    cpu_idle: int
    network_rx: int
    network_tx: int
    disk_read_sectors: int
    disk_write_sectors: int
    disk_busy_ms: int


class HostMetrics:
    def __init__(self, disk_path: Path) -> None:
        self.disk_path = disk_path
        self.previous: Optional[HostSample] = None
        self.disk_device: Optional[Tuple[int, int]] = None
        try:
            device_id = os.stat(self.disk_path).st_dev
            self.disk_device = (os.major(device_id), os.minor(device_id))
        except OSError:
            pass

    def collect(self) -> Dict[str, Any]:
        now = time.monotonic()
        try:
            cpu_total, cpu_idle = parse_proc_stat(read_text("/proc/stat"))
        except (ValueError, TypeError):
            cpu_total, cpu_idle = 0, 0
        memory_total, memory_available = parse_meminfo(read_text("/proc/meminfo"))
        network_rx, network_tx = parse_net_dev(read_text("/proc/net/dev"))
        disk_read, disk_write, disk_busy = parse_diskstats(
            read_text("/proc/diskstats"), self.disk_device)
        current = HostSample(now, cpu_total, cpu_idle, network_rx, network_tx,
                             disk_read, disk_write, disk_busy)
        result: Dict[str, Any] = {
            "cpu_percent": None,
            "network_rx_per_second": None,
            "network_tx_per_second": None,
            "disk_read_per_second": None,
            "disk_write_per_second": None,
            "disk_busy_percent": None,
            "memory_total": memory_total,
            "memory_available": memory_available,
        }
        previous = self.previous
        if previous is not None:
            elapsed = max(0.001, now - previous.captured)
            total_delta = cpu_total - previous.cpu_total
            idle_delta = cpu_idle - previous.cpu_idle
            if total_delta > 0:
                result["cpu_percent"] = max(0.0, min(100.0,
                    100.0 * (total_delta - idle_delta) / total_delta))
            result["network_rx_per_second"] = max(0.0, (network_rx - previous.network_rx) / elapsed)
            result["network_tx_per_second"] = max(0.0, (network_tx - previous.network_tx) / elapsed)
            result["disk_read_per_second"] = max(0.0, (disk_read - previous.disk_read_sectors) * 512 / elapsed)
            result["disk_write_per_second"] = max(0.0, (disk_write - previous.disk_write_sectors) * 512 / elapsed)
            result["disk_busy_percent"] = max(0.0, min(100.0,
                (disk_busy - previous.disk_busy_ms) / (elapsed * 10.0)))
        self.previous = current
        try:
            filesystem = os.statvfs(self.disk_path)
            result["disk_total"] = filesystem.f_blocks * filesystem.f_frsize
            result["disk_available"] = filesystem.f_bavail * filesystem.f_frsize
        except OSError:
            result["disk_total"] = 0
            result["disk_available"] = 0
        return result


class HttpJsonClient:
    def __init__(self, base_url: str, token: str = "", timeout: float = 3.0) -> None:
        parsed = urllib.parse.urlsplit(base_url.rstrip("/"))
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            raise ValueError("URL must be an absolute HTTP(S) URL")
        if parsed.username or parsed.password or parsed.query or parsed.fragment:
            raise ValueError("URL contains unsupported components")
        if parsed.path not in {"", "/"}:
            raise ValueError("URL path must be empty or /")
        try:
            port = parsed.port
        except ValueError as error:
            raise ValueError("URL port is invalid") from error
        self.scheme = parsed.scheme
        self.host = parsed.hostname
        self.port = port or (443 if parsed.scheme == "https" else 80)
        self.token = token
        self.timeout = timeout

    def request(self, method: str, path: str,
                body: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        if not path.startswith("/") or "\r" in path or "\n" in path:
            raise ValueError("invalid HTTP path")
        connection_type = http.client.HTTPSConnection if self.scheme == "https" else http.client.HTTPConnection
        connection = connection_type(self.host, self.port, timeout=self.timeout)
        headers = {"Accept": "application/json", "Connection": "close"}
        payload: Optional[bytes] = None
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        if body is not None:
            payload = json.dumps(body, separators=(",", ":")).encode("utf-8")
            headers["Content-Type"] = "application/json"
        try:
            connection.request(method, path, body=payload, headers=headers)
            response = connection.getresponse()
            declared = response.getheader("Content-Length")
            if declared is not None and int(declared) > 4 * 1024 * 1024:
                raise ValueError("HTTP response is too large")
            raw = response.read(4 * 1024 * 1024 + 1)
            if len(raw) > 4 * 1024 * 1024:
                raise ValueError("HTTP response is too large")
            # Health/current-round may legitimately return 503 JSON.
            if response.status not in {200, 503}:
                raise RuntimeError(f"HTTP {response.status}")
            value = json.loads(raw.decode("utf-8"))
            if not isinstance(value, dict):
                raise ValueError("HTTP JSON root is not an object")
            return value
        finally:
            connection.close()

    def get(self, path: str) -> Dict[str, Any]:
        return self.request("GET", path)

    def post(self, path: str, body: Dict[str, Any]) -> Dict[str, Any]:
        return self.request("POST", path, body)


@dataclasses.dataclass(frozen=True)
class StatusSnapshot:
    captured_at: str
    summary: Dict[str, Any]
    ready: Dict[str, Any]
    verifier: Dict[str, Any]
    persistence: Dict[str, Any]
    top: Dict[str, Any]
    high: Dict[str, Any]
    monerod: Dict[str, Any]
    host: Dict[str, Any]
    error: Optional[str] = None


class StatusCollector:
    def __init__(self, api_url: str, token: str, monero_rpc_url: str,
                 disk_path: Path) -> None:
        self.api = HttpJsonClient(api_url, token)
        self.monero = HttpJsonClient(monero_rpc_url)
        self.host = HostMetrics(disk_path)
        self.last_api: Dict[str, Dict[str, Any]] = {}
        self.last_monerod: Dict[str, Any] = {}

    def _api_get(self, path: str, errors: List[str]) -> Dict[str, Any]:
        cache = getattr(self, "last_api", None)
        if cache is None:
            cache = {}
            self.last_api = cache
        try:
            value = self.api.get(path)
            cache[path] = value
            return value
        except Exception as error:
            errors.append(f"{path}: {error}")
            return cache.get(path, {})

    def collect(self) -> StatusSnapshot:
        captured = _datetime.datetime.now(_datetime.timezone.utc).isoformat().replace("+00:00", "Z")
        # Sources are deliberately independent. A pool endpoint failure must
        # not freeze host load or monerod peer/network state.
        errors: List[str] = []
        try:
            host = self.host.collect()
        except Exception as error:
            errors.append(f"host: {error}")
            host = {}
        summary = self._api_get("/v1/summary", errors)
        data = summary.get("data") if isinstance(summary.get("data"), dict) else {}
        round_data = data.get("round") if isinstance(data.get("round"), dict) else {}
        round_id = nonnegative_int(round_data.get("id"))
        top_path = f"/v1/shares/top?round_id={round_id}" if round_id else "/v1/shares/top"
        ready = self._api_get("/v1/health/ready", errors)
        verifier = self._api_get("/v1/verifier", errors)
        persistence = self._api_get("/v1/persistence", errors)
        top = self._api_get(top_path, errors)
        high = self._api_get("/v1/shares/recent-high", errors)
        try:
            monerod_response = self.monero.post("/get_info", {})
            monerod = monerod_response.get("result", monerod_response)
            if not isinstance(monerod, dict):
                monerod = {}
            self.last_monerod = monerod
        except Exception as error:
            errors.append(f"monerod: {error}")
            monerod = dict(getattr(self, "last_monerod", {}))
            monerod["error"] = str(error)
        return StatusSnapshot(captured, summary, ready, verifier, persistence,
                              top, high, monerod, host,
                              "; ".join(errors) if errors else None)


def unwrap(envelope: Mapping[str, Any]) -> Dict[str, Any]:
    data = envelope.get("data")
    return data if isinstance(data, dict) else {}


def human_si(value: Any, unit: str = "") -> str:
    number = finite_float(value)
    if number is None:
        return "?"
    magnitude = abs(number)
    suffixes = ((1e18, "E"), (1e15, "P"), (1e12, "T"), (1e9, "G"),
                (1e6, "M"), (1e3, "k"))
    scale, suffix = 1.0, ""
    for candidate, candidate_suffix in suffixes:
        if magnitude >= candidate:
            scale, suffix = candidate, candidate_suffix
            break
    scaled = number / scale
    if scale == 1:
        text = f"{scaled:.0f}"
    elif abs(scaled) >= 100:
        text = f"{scaled:.0f}"
    elif abs(scaled) >= 10:
        text = f"{scaled:.1f}"
    else:
        text = f"{scaled:.2f}"
    return f"{text} {suffix}{unit}".rstrip()


def human_bytes(value: Any) -> str:
    number = finite_float(value)
    if number is None:
        return "?"
    for scale, suffix in ((1024 ** 4, "TiB"), (1024 ** 3, "GiB"),
                          (1024 ** 2, "MiB"), (1024, "KiB")):
        if abs(number) >= scale:
            scaled = number / scale
            return f"{scaled:.1f} {suffix}" if abs(scaled) >= 10 else f"{scaled:.2f} {suffix}"
    return f"{number:.0f} B"


def human_duration(value: Any) -> str:
    seconds = nonnegative_int(value) or 0
    days, seconds = divmod(seconds, 86400)
    hours, seconds = divmod(seconds, 3600)
    minutes, seconds = divmod(seconds, 60)
    parts = []
    if days:
        parts.append(f"{days}d")
    if hours:
        parts.append(f"{hours}h")
    if minutes:
        parts.append(f"{minutes}m")
    if seconds or not parts:
        parts.append(f"{seconds}s")
    return " ".join(parts)


def progress_bar(percent: Optional[float], width: int = 18) -> str:
    if percent is None:
        return "[n/a]"
    bounded = max(0.0, min(100.0, percent))
    filled = int(math.floor(bounded * width / 100.0))
    return "[" + "#" * filled + "-" * (width - filled) + "]"


def round_progress(snapshot: Optional[StatusSnapshot]) -> Tuple[Optional[float], Optional[float], int, int]:
    if snapshot is None:
        return None, None, 0, 0
    summary = unwrap(snapshot.summary)
    round_data = summary.get("round") if isinstance(summary.get("round"), dict) else {}
    effort_data = round_data.get("effort") if isinstance(round_data.get("effort"), dict) else {}
    effort = finite_float(effort_data.get("value"))
    if effort is None:
        return None, None, 0, 0
    chance = 100.0 * (1.0 - math.exp(-max(0.0, effort) / 100.0))
    chance_tier = max(0, min(9, int(chance // 10)))
    effort_tier = max(0, min(9, int(max(0.0, effort) // 100)))
    return effort, chance, effort_tier, chance_tier


def ranked_share_line(index: int, item: Mapping[str, Any]) -> str:
    timestamp = str(item.get("completed_at", item.get("received_at", "")))
    if timestamp:
        timestamp = timestamp.replace("T", " ").rstrip("Z")[:19] + " UTC"
    else:
        timestamp = "time ?"
    worker = item.get("worker_id")
    worker_text = f"  worker#{worker}" if worker is not None else ""
    return (f"  {index}. {human_si(item.get('actual_difficulty'))}  {timestamp}  "
            f"share#{item.get('id', '?')}{worker_text}")


def format_time(value: Any) -> str:
    text = str(value or "")
    if "T" in text:
        text = text.split("T", 1)[1]
    return text.rstrip("Z")[:12] or "--:--:--"


def format_event(entry: EventEntry) -> str:
    event = entry.event
    fields = event_fields(event)
    code = str(event.get("code", "unknown"))
    prefix = format_time(event.get("time"))
    if code == "share.completed":
        status = str(fields.get("status", "unknown")).upper()
        actual = fields.get("actual_difficulty")
        assigned = finite_float(fields.get("assigned_difficulty"))
        difficulty = finite_float(actual)
        parts = [prefix, status, human_si(actual, "diff")]
        if difficulty is not None and assigned:
            parts.append(f"{difficulty / assigned:.2f}x")
        if fields.get("height") is not None:
            parts.append(f"h{fields['height']}")
        if fields.get("share_id") is not None:
            parts.append(f"share#{fields['share_id']}")
        duration = finite_float(fields.get("duration_us"))
        verifier_ns = finite_float(fields.get("verifier_total_ns"))
        if duration is not None:
            parts.append(f"e2e {duration / 1000:.2f}ms")
        if verifier_ns is not None:
            parts.append(f"rx {verifier_ns / 1e6:.2f}ms")
        return "  ".join(parts)
    if code == "template.refreshed":
        return (f"{prefix}  TEMPLATE  {fields.get('fetch_reason', '?')}  "
                f"h{fields.get('height', '?')}  template#{fields.get('template_id', '?')}  "
                f"net {human_si(fields.get('network_difficulty'), 'diff')}")
    if code == "job.queued":
        return (f"{prefix}  JOB  h{fields.get('height', '?')}  "
                f"job#{fields.get('job_id', '?')}  conn#{fields.get('connection_id', '?')}  "
                f"diff {human_si(fields.get('assigned_difficulty'))}")
    if code.startswith("connection."):
        return (f"{prefix}  {code.upper()}  conn#{fields.get('connection_id', '?')}  "
                f"worker#{fields.get('worker_id', '?')}  {fields.get('reason_code', '')}").rstrip()
    allow = ("status", "state", "reason_code", "height", "candidate_id",
             "share_id", "delivery_id", "attempt", "exit_code")
    details = "  ".join(f"{key}={fields[key]}" for key in allow
                        if fields.get(key) is not None)
    return f"{prefix}  {code.upper()}" + (f"  {details}" if details else "")


def plain_color_enabled(args: argparse.Namespace) -> bool:
    return (args.ui == "tty" and sys.stdout.isatty() and
            os.environ.get("TERM", "dumb") != "dumb" and
            "NO_COLOR" not in os.environ)


def plain_style(text: str, role: str, theme: str, enabled: bool) -> str:
    if not enabled or not text:
        return text
    windows = theme in {"windows-classic", "windows-classic-tty"}
    palettes = ({
        "title": "1;37;44", "section": "30;47", "ok": "1;32",
        "warn": "1;33", "error": "1;31", "dim": "2;37",
        "green": "1;32", "yellow": "1;33", "red": "1;31",
    } if windows else {
        "title": "1;36", "section": "1;36", "ok": "1;32",
        "warn": "1;33", "error": "1;31", "dim": "2",
        "green": "1;32", "yellow": "1;33", "red": "1;31",
    })
    code = palettes.get(role, "0")
    return f"\x1b[{code}m{text}\x1b[0m"


def plain_status_line(line: str, snapshot: StatusSnapshot,
                      theme: str, enabled: bool) -> str:
    sections = {"NODE", "MINING", "VERIFIER", "HOST", "RECENT HIGH SHARES"}
    role = "body"
    if line in sections or line.startswith("ROUND #") or line.startswith("CURRENT ROUND"):
        role = "section"
    elif "SOURCE WARNING" in line:
        role = "warn"
    elif "NOT READY" in line or " failed " in line:
        role = "error"
    elif line.startswith("  Luck"):
        _effort, _chance, _effort_tier, chance_tier = round_progress(snapshot)
        role = "green" if chance_tier < 4 else ("yellow" if chance_tier < 7 else "red")
    elif line.startswith("  Effort"):
        _effort, _chance, effort_tier, _chance_tier = round_progress(snapshot)
        role = "green" if effort_tier < 4 else ("yellow" if effort_tier < 7 else "red")
    elif "READY" in line or " enabled" in line:
        role = "ok"
    return plain_style(line, role, theme, enabled)


def plain_event_line(entry: EventEntry, theme: str, enabled: bool) -> str:
    group = event_group(entry)
    severity = str(entry.event.get("severity", "")).lower()
    role = "body"
    if severity in {"error", "fatal", "critical"}:
        role = "error"
    elif group == "candidates":
        role = "warn"
    elif group == "exceptional-shares":
        role = "warn"
    elif group == "high-shares":
        role = "ok"
    elif group in {"templates", "system"}:
        role = "section"
    return plain_style(format_event(entry), role, theme, enabled)


def snapshot_status_lines(snapshot: Optional[StatusSnapshot], width: int = 58) -> List[str]:
    if snapshot is None:
        return ["STATUS", "", "Waiting for pool, monerod and host samples..."]
    summary = unwrap(snapshot.summary)
    ready = unwrap(snapshot.ready)
    verifier = unwrap(snapshot.verifier)
    storage = unwrap(snapshot.persistence)
    server = summary.get("server") if isinstance(summary.get("server"), dict) else {}
    daemon = summary.get("daemon") if isinstance(summary.get("daemon"), dict) else {}
    connections = summary.get("connections") if isinstance(summary.get("connections"), dict) else {}
    workers = summary.get("workers") if isinstance(summary.get("workers"), dict) else {}
    hashrate = summary.get("hashrate") if isinstance(summary.get("hashrate"), dict) else {}
    shares = summary.get("shares") if isinstance(summary.get("shares"), dict) else {}
    round_data = summary.get("round") if isinstance(summary.get("round"), dict) else {}
    candidates = summary.get("candidates") if isinstance(summary.get("candidates"), dict) else {}
    monero = snapshot.monerod
    host = snapshot.host
    network_difficulty = (monero.get("wide_difficulty") or monero.get("difficulty") or
                          daemon.get("network_difficulty"))
    effort_data = round_data.get("effort") if isinstance(round_data.get("effort"), dict) else {}
    effort = finite_float(effort_data.get("value"))
    chance = None if effort is None else 100.0 * (1.0 - math.exp(-effort / 100.0))
    cycle = None if effort is None else effort % 100.0
    verifier_stats = verifier.get("stats") if isinstance(verifier.get("stats"), dict) else {}
    verifier_config = verifier.get("configuration") if isinstance(verifier.get("configuration"), dict) else {}
    seeds = verifier.get("seeds") if isinstance(verifier.get("seeds"), list) else []
    active_seed = next((seed for seed in seeds if isinstance(seed, dict) and seed.get("state") == "current"), None)
    memory_total = finite_float(host.get("memory_total")) or 0
    memory_available = finite_float(host.get("memory_available")) or 0
    memory_used = None if memory_total <= 0 else 100.0 * (memory_total - memory_available) / memory_total
    disk_total = finite_float(host.get("disk_total")) or 0
    disk_available = finite_float(host.get("disk_available")) or 0
    disk_used = None if disk_total <= 0 else 100.0 * (disk_total - disk_available) / disk_total
    large_pages = (active_seed or {}).get("all_vms_use_large_pages")
    large_pages_text = "yes" if large_pages is True else ("no" if large_pages is False else "?")
    tier = 1 if effort is None else min(10, max(1, int(max(0.0, effort) // 100) + 1))
    lines = [
        "NODE",
        f"  {'READY' if ready.get('ready') else 'NOT READY'}  {server.get('network', '?')}  h{daemon.get('height', monero.get('height', '?'))}  up {human_duration(server.get('uptime_seconds'))}",
        f"  RPC {daemon.get('rpc', '?')}  ZMQ {daemon.get('zmq', '?')}  synced {monero.get('synchronized', '?')}",
        f"  Peers {monero.get('outgoing_connections_count', '?')} out / {monero.get('incoming_connections_count', '?')} in  net {human_si(network_difficulty, 'diff')}",
        "",
        "MINING",
        f"  {connections.get('active', 0)} connections / {workers.get('active', 0)} workers  [{hashrate.get('source', '?')}]",
        f"  1m {human_si(hashrate.get('1m'), 'H/s')}  5m {human_si(hashrate.get('5m'), 'H/s')}  1h {human_si(hashrate.get('1h'), 'H/s')}",
        f"  shares {shares.get('accepted', 0)}/{shares.get('total', 0)}  stale {shares.get('stale', 0)}  dup {shares.get('duplicate', 0)}",
        "",
        f"ROUND #{round_data.get('id', '?')}  {round_data.get('state', '?')}",
        f"  Luck   {progress_bar(chance)}  {chance:.6f}%" if chance is not None else "  Luck   [n/a]",
        f"  Effort {progress_bar(cycle)}  {cycle:.6f}% cycle / {effort:.6f}% total  tier {tier}/10" if effort is not None and cycle is not None else "  Effort [n/a]",
        f"  Work {human_si(round_data.get('estimated_hashes'), 'H')}  candidates {candidates.get('accepted', 0)}/{candidates.get('total', 0)}",
        "",
        "VERIFIER",
        f"  {'enabled' if verifier.get('enabled') else 'disabled'}  {verifier.get('provenance', '?')}/{verifier_config.get('memory_mode', '?')}  workers {verifier_stats.get('workers', '?')}",
        f"  seed {str((active_seed or {}).get('seed_id', verifier_stats.get('active_seed_id', '?')))[:12]}  ready {verifier_stats.get('seeds_ready', '?')} preparing {verifier_stats.get('seeds_preparing', '?')}",
        f"  huge pages {large_pages_text}  pending {verifier_stats.get('pending', 0)}  failed {verifier_stats.get('failed', 0)}",
        "",
        "HOST",
        f"  CPU {host.get('cpu_percent'):.1f}%" if host.get("cpu_percent") is not None else "  CPU sampling...",
        f"  RAM {memory_used:.1f}% / {human_bytes(memory_available)} free" if memory_used is not None else "  RAM unavailable",
        f"  NET rx {human_bytes(host.get('network_rx_per_second'))}/s  tx {human_bytes(host.get('network_tx_per_second'))}/s",
        f"  DISK {disk_used:.1f}% / {human_bytes(disk_available)} free  busy {host.get('disk_busy_percent'):.1f}%" if disk_used is not None and host.get("disk_busy_percent") is not None else "  DISK sampling...",
        f"       read {human_bytes(host.get('disk_read_per_second'))}/s  write {human_bytes(host.get('disk_write_per_second'))}/s",
        f"  DB {human_bytes(storage.get('database_bytes'))} + WAL {human_bytes(storage.get('wal_bytes'))}  queue {storage.get('writer_queue_items', 0)}",
        "",
        f"CURRENT ROUND TOP SHARES  #{round_data.get('id', '?')}",
    ]
    if snapshot.error:
        lines[1:1] = [f"  SOURCE WARNING  {snapshot.error}", ""]
    top_rows = snapshot.top.get("data") if isinstance(snapshot.top.get("data"), list) else []
    for index, item in enumerate(top_rows[:5], 1):
        if isinstance(item, dict):
            lines.append(ranked_share_line(index, item))
    if not top_rows:
        lines.append("  (none)")
    lines.extend(("", "RECENT HIGH SHARES"))
    high_rows = snapshot.high.get("data") if isinstance(snapshot.high.get("data"), list) else []
    for index, item in enumerate(high_rows[:5], 1):
        if isinstance(item, dict):
            lines.append(ranked_share_line(index, item))
    if not high_rows:
        lines.append("  (none)")
    return lines


class WorkerState:
    def __init__(self) -> None:
        self.stop = threading.Event()
        self.wake_status = threading.Event()
        self.status_queue: "queue.Queue[StatusSnapshot]" = queue.Queue(maxsize=1)
        self.event_queue: "queue.Queue[EventEntry]" = queue.Queue(maxsize=4096)
        self.status_error: Optional[str] = None
        self.event_error: Optional[str] = None
        self.sequence = 0
        self.lock = threading.Lock()

    def next_sequence(self) -> int:
        with self.lock:
            self.sequence += 1
            return self.sequence


def replace_latest(target: "queue.Queue[Any]", value: Any) -> None:
    try:
        target.put_nowait(value)
        return
    except queue.Full:
        pass
    try:
        target.get_nowait()
    except queue.Empty:
        pass
    try:
        target.put_nowait(value)
    except queue.Full:
        pass


def status_worker(state: WorkerState, collector: StatusCollector,
                  interval_getter) -> None:
    previous: Optional[StatusSnapshot] = None
    while not state.stop.is_set():
        try:
            current = collector.collect()
            previous = current
            state.status_error = None
            replace_latest(state.status_queue, current)
        except Exception as error:
            state.status_error = str(error)
            if previous is not None:
                replace_latest(state.status_queue,
                    dataclasses.replace(previous, error=str(error)))
        interval = max(0.5, float(interval_getter()))
        state.wake_status.wait(interval)
        state.wake_status.clear()


def event_worker(state: WorkerState, path: Path, from_start: bool,
                 sampler: AdaptiveSampler, filter_getter,
                 configuration_getter=None) -> None:
    follower = JsonlFollower(path, from_start)
    last_flush = time.monotonic()
    try:
        while not state.stop.is_set():
            if configuration_getter is not None:
                rate, floor = configuration_getter()
                sampler.rate = max(0.05, min(1000.0, float(rate)))
                sampler.min_share_difficulty = max(1, int(floor))
            records = follower.poll()
            if follower.handle is None:
                state.event_error = f"waiting for {path}"
            else:
                state.event_error = None
            for raw in records:
                event = parse_json_event(raw)
                if event is None:
                    continue
                entry = EventEntry(state.next_sequence(), event)
                active_filter: EventFilter = filter_getter()
                if not active_filter.matches(entry):
                    continue
                disposition = sampler.add(entry)
                if disposition == "priority":
                    try:
                        state.event_queue.put_nowait(entry)
                    except queue.Full:
                        try:
                            state.event_queue.get_nowait()
                            state.event_queue.put_nowait(entry)
                        except (queue.Empty, queue.Full):
                            pass
            now = time.monotonic()
            if now - last_flush >= SAMPLING_WINDOW:
                for entry in sampler.flush(now - last_flush):
                    # A runtime checkbox change can happen while an item is in
                    # the current one-second reservoir. Recheck at emission so
                    # a disabled category disappears immediately.
                    if not filter_getter().matches(entry):
                        continue
                    try:
                        state.event_queue.put_nowait(entry)
                    except queue.Full:
                        try:
                            state.event_queue.get_nowait()
                            state.event_queue.put_nowait(entry)
                        except (queue.Empty, queue.Full):
                            pass
                last_flush = now
            state.stop.wait(0.05)
    finally:
        follower.close()


@dataclasses.dataclass
class RuntimeOptions:
    interval: float
    event_rate: float
    min_share_difficulty: int
    layout: str
    view: str
    theme: str


@dataclasses.dataclass
class DashboardState:
    view: str
    layout: str
    reverse: bool
    theme: str
    interval: float
    event_rate: float
    min_share_difficulty: int
    event_filter: EventFilter
    history: EventBuffer
    focus: str = "events"
    paused: bool = False
    unseen: int = 0
    status_scroll: int = 0
    latest_status: Optional[StatusSnapshot] = None
    export_in_progress: bool = False
    toast: str = ""
    toast_until: float = 0.0

    def notify(self, message: str, seconds: float = 4.0) -> None:
        self.toast = message
        self.toast_until = time.monotonic() + seconds


class ThemeManager:
    def __init__(self, screen: Any, name: str) -> None:
        colors = getattr(curses, "COLORS", 0) if curses.has_colors() else 0
        if name == "auto":
            name = "tokyo-neon" if colors >= 256 else "black"
        self.theme = select_theme(name, colors)
        self.attributes: Dict[str, int] = {}
        if curses.has_colors():
            try:
                curses.use_default_colors()
            except curses.error:
                pass
            for pair_number, (role, (foreground, background)) in enumerate(self.theme.colors.items(), 1):
                try:
                    curses.init_pair(pair_number, foreground, background)
                    self.attributes[role] = curses.color_pair(pair_number)
                except curses.error:
                    self.attributes[role] = 0
            background = self.theme.colors.get("body", (7, -1))[1]
            palette = (46, 40, 34, 70, 106, 142, 178, 214, 208, 196)
            if colors < 256:
                palette = (2, 2, 2, 2, 3, 3, 3, 1, 1, 1)
            first_pair = len(self.theme.colors) + 1
            for tier, foreground in enumerate(palette):
                try:
                    curses.init_pair(first_pair + tier, foreground, background)
                    self.attributes[f"tier{tier}"] = (
                        curses.color_pair(first_pair + tier) | curses.A_BOLD)
                except curses.error:
                    self.attributes[f"tier{tier}"] = self.attributes.get(
                        "ok" if tier < 4 else ("warn" if tier < 7 else "error"), 0)
        for role in self.theme.colors:
            self.attributes.setdefault(role, 0)
        self.attributes["focus"] |= curses.A_BOLD
        self.attributes["title"] |= curses.A_BOLD
        self.attributes["error"] |= curses.A_BOLD

    def attr(self, role: str) -> int:
        return self.attributes.get(role, 0)


def safe_addstr(screen: Any, y: int, x: int, text: str, attr: int = 0,
                maximum: Optional[int] = None) -> None:
    rows, columns = screen.getmaxyx()
    if y < 0 or x < 0 or y >= rows or x >= columns:
        return
    allowed = max(0, columns - x - (1 if y == rows - 1 else 0))
    if maximum is not None:
        allowed = min(allowed, maximum)
    if allowed <= 0:
        return
    value = str(text)
    if len(value) > allowed:
        value = value[:max(0, allowed - 1)] + ("…" if allowed else "")
    try:
        screen.addstr(y, x, value, attr)
    except curses.error:
        pass


def draw_box(screen: Any, rect: Rect, title: str, manager: ThemeManager,
             focused: bool) -> None:
    if rect.height <= 1 or rect.width <= 1:
        return
    top_left, top_right, bottom_left, bottom_right, horizontal, vertical = manager.theme.border
    attr = manager.attr("focus" if focused else "border")
    safe_addstr(screen, rect.y, rect.x, top_left + horizontal * max(0, rect.width - 2) + top_right, attr, rect.width)
    safe_addstr(screen, rect.y + rect.height - 1, rect.x,
                bottom_left + horizontal * max(0, rect.width - 2) + bottom_right, attr, rect.width)
    for y in range(rect.y + 1, rect.y + rect.height - 1):
        safe_addstr(screen, y, rect.x, vertical, attr, 1)
        safe_addstr(screen, y, rect.x + rect.width - 1, vertical, attr, 1)
    label = f" {title} "
    safe_addstr(screen, rect.y, rect.x + 2, label, manager.attr("focus" if focused else "title"), max(0, rect.width - 4))


def draw_status(screen: Any, rect: Rect, state: DashboardState,
                manager: ThemeManager) -> None:
    title = "STATUS"
    if state.latest_status and state.latest_status.error:
        title += " [STALE]"
    draw_box(screen, rect, title, manager, state.focus == "status")
    inner_height = max(0, rect.height - 2)
    lines = snapshot_status_lines(state.latest_status, max(0, rect.width - 2))
    max_scroll = max(0, len(lines) - inner_height)
    state.status_scroll = max(0, min(state.status_scroll, max_scroll))
    _effort, _chance, effort_tier, chance_tier = round_progress(state.latest_status)
    for offset, line in enumerate(lines[state.status_scroll:state.status_scroll + inner_height]):
        role = "accent" if line and not line.startswith(" ") else "body"
        if "NOT READY" in line:
            role = "error"
        elif line.startswith("  Luck"):
            role = f"tier{chance_tier}"
        elif line.startswith("  Effort"):
            role = f"tier{effort_tier}"
        elif "SOURCE WARNING" in line:
            role = "warn"
        elif "READY" in line or " enabled" in line:
            role = "ok"
        safe_addstr(screen, rect.y + 1 + offset, rect.x + 1, line,
                    manager.attr(role), max(0, rect.width - 2))


def draw_events(screen: Any, rect: Rect, state: DashboardState,
                manager: ThemeManager) -> None:
    mode = f"PAUSED +{state.unseen}" if state.paused else "LIVE"
    title = (f"EVENT STREAM  {mode}  {state.event_rate:g}/s  "
             f"filter {len(state.event_filter.enabled)}/{len(FILTER_KEYS)}  "
             f"selected {len(state.history.selected_events())}")
    draw_box(screen, rect, title, manager, state.focus == "events")
    inner_height = max(0, rect.height - 2)
    rows = state.history.visible(inner_height, state.event_filter)
    cursor = state.history.current(state.event_filter)
    for offset, entry in enumerate(rows):
        selected = state.history.is_selected(entry.sequence)
        is_cursor = cursor is not None and cursor.sequence == entry.sequence
        marker = (manager.theme.cursor_marker if is_cursor else " ") + (
            manager.theme.selected_marker if selected else " ")
        group = event_group(entry)
        role = "body"
        if selected:
            role = "select"
        elif group == "candidates":
            role = "accent"
        elif group == "exceptional-shares":
            role = "warn"
        elif group == "high-shares":
            role = "ok"
        elif str(entry.event.get("severity", "")).lower() in {"error", "fatal", "critical"}:
            role = "error"
        safe_addstr(screen, rect.y + 1 + offset, rect.x + 1,
                    marker + " " + format_event(entry), manager.attr(role),
                    max(0, rect.width - 2))


def draw_frame(screen: Any, state: DashboardState, manager: ThemeManager,
               event_error: Optional[str] = None) -> None:
    screen.erase()
    rows, columns = screen.getmaxyx()
    ready = False
    height: Any = "?"
    network = "?"
    if state.latest_status:
        ready = bool(unwrap(state.latest_status.ready).get("ready"))
        summary = unwrap(state.latest_status.summary)
        daemon = summary.get("daemon") if isinstance(summary.get("daemon"), dict) else {}
        server = summary.get("server") if isinstance(summary.get("server"), dict) else {}
        height = daemon.get("height", "?")
        network = str(server.get("network", "?"))
    heading = f" MONERO SOLO STRATUM  {'READY' if ready else 'WAITING'}  {network}  h{height} "
    safe_addstr(screen, 0, 0, heading, manager.attr("title"), columns)
    layout = resolve_layout(state.view, state.layout, state.reverse, rows, columns)
    if layout.status:
        draw_status(screen, layout.status, state, manager)
    if layout.events:
        draw_events(screen, layout.events, state, manager)
    footer = state.toast if state.toast and time.monotonic() < state.toast_until else (
        "Tab focus  1 status  2 events  3 both  r reverse  p pause  "
        "f filter  o options  Space select  e export  ? help  q quit"
    )
    if event_error and not state.toast:
        footer = event_error
    safe_addstr(screen, max(0, rows - 1), 0, footer,
                manager.attr("warn" if state.toast else "dim"), columns)
    try:
        screen.refresh()
    except curses.error:
        pass


def draw_overlay(screen: Any, title: str, lines: Sequence[str],
                 manager: ThemeManager, width: Optional[int] = None) -> Rect:
    rows, columns = screen.getmaxyx()
    desired_width = width or max([len(title) + 8] + [len(line) + 4 for line in lines])
    box_width = max(20, min(columns - 2, desired_width)) if columns >= 3 else columns
    box_height = max(3, min(rows - 2, len(lines) + 2)) if rows >= 3 else rows
    y = max(0, (rows - box_height) // 2)
    x = max(0, (columns - box_width) // 2)
    rect = Rect(y, x, box_height, box_width)
    try:
        for row in range(y, min(rows, y + box_height)):
            safe_addstr(screen, row, x, " " * box_width, manager.attr("panel"), box_width)
    except curses.error:
        pass
    draw_box(screen, rect, title, manager, True)
    for index, line in enumerate(lines[:max(0, box_height - 2)]):
        safe_addstr(screen, y + 1 + index, x + 2, line,
                    manager.attr("body"), max(0, box_width - 4))
    try:
        screen.refresh()
    except curses.error:
        pass
    return rect


def modal_message(screen: Any, title: str, lines: Sequence[str],
                  manager: ThemeManager) -> None:
    draw_overlay(screen, title, list(lines) + ["", "Esc / Enter to close"], manager)
    while True:
        key = screen.getch()
        if key in (27, 10, 13, curses.KEY_ENTER, ord("q")):
            return


def modal_filter(screen: Any, state: DashboardState,
                 manager: ThemeManager) -> bool:
    model = CategoryChecklist(state.event_filter.enabled)
    while True:
        lines = []
        for index, (category, label) in enumerate(FILTER_GROUPS):
            cursor = ">" if index == model.cursor else " "
            checked = "x" if category in model.enabled else " "
            lines.append(f"{cursor} [{checked}] {label}")
        lines.extend(("", "Space toggle   A all   N none",
                      "Enter apply     Esc cancel"))
        draw_overlay(screen, "STREAM FILTER", lines, manager, 52)
        key = screen.getch()
        outcome = model.handle(key)
        if outcome == "apply":
            state.event_filter = EventFilter(model.enabled)
            state.history.ensure_visible(state.event_filter)
            state.history.follow = not state.paused
            state.notify(f"Filter applied: {len(model.enabled)}/{len(FILTER_KEYS)} categories")
            return True
        if outcome == "cancel":
            return False


def modal_options(screen: Any, state: DashboardState,
                  manager: ThemeManager) -> bool:
    values = RuntimeOptions(state.interval, state.event_rate,
                            state.min_share_difficulty, state.layout,
                            state.view, state.theme)
    cursor = 0
    layouts = ("auto", "horizontal", "vertical")
    views = ("both", "status", "events")
    themes = tuple(THEMES)
    while True:
        fields = (
            ("Status refresh", f"{values.interval:g} s"),
            ("Stream target", f"{values.event_rate:g} rows/s"),
            ("Accepted-share floor", human_si(values.min_share_difficulty, "diff")),
            ("Layout", values.layout),
            ("View", values.view),
            ("Theme", values.theme),
        )
        lines = [f"{'>' if index == cursor else ' '} {label:<22} [ {value} ]"
                 for index, (label, value) in enumerate(fields)]
        lines.extend(("", "Up/Down field   Left/Right change",
                      "Enter apply      Esc cancel"))
        draw_overlay(screen, "RUNTIME OPTIONS", lines, manager, 58)
        key = screen.getch()
        if key in (27,):
            return False
        if key in (10, 13, curses.KEY_ENTER):
            state.interval = values.interval
            state.event_rate = values.event_rate
            state.min_share_difficulty = values.min_share_difficulty
            state.layout = values.layout
            state.view = values.view
            state.theme = values.theme
            if state.view == "status":
                state.focus = "status"
            elif state.view == "events":
                state.focus = "events"
            state.notify("Runtime options applied")
            return True
        if key in (curses.KEY_UP, ord("k")):
            cursor = (cursor - 1) % len(fields)
            continue
        if key in (curses.KEY_DOWN, ord("j")):
            cursor = (cursor + 1) % len(fields)
            continue
        direction = -1 if key in (curses.KEY_LEFT, ord("h"), ord("-")) else (
            1 if key in (curses.KEY_RIGHT, ord("l"), ord("+")) else 0)
        if not direction:
            continue
        if cursor == 0:
            values.interval = max(1.0, min(300.0, values.interval + direction))
        elif cursor == 1:
            values.event_rate = max(0.05, min(1000.0,
                values.event_rate + direction * (0.25 if values.event_rate < 5 else 1.0)))
        elif cursor == 2:
            values.min_share_difficulty = max(1,
                int(values.min_share_difficulty * (2 if direction > 0 else 0.5)))
        elif cursor == 3:
            index = (layouts.index(values.layout) + direction) % len(layouts)
            values.layout = layouts[index]
        elif cursor == 4:
            index = (views.index(values.view) + direction) % len(views)
            values.view = views[index]
        elif cursor == 5:
            current = themes.index(values.theme) if values.theme in themes else 0
            values.theme = themes[(current + direction) % len(themes)]


def text_dialog(screen: Any, title: str, prompt: str, manager: ThemeManager,
                initial: str = "") -> Optional[str]:
    value = list(initial)
    cursor = len(value)
    try:
        curses.curs_set(1)
    except curses.error:
        pass
    try:
        while True:
            display = "".join(value)
            lines = [prompt, "", display, "", "Enter confirm   Esc cancel"]
            rect = draw_overlay(screen, title, lines, manager,
                                min(max(54, len(display) + 8), max(20, screen.getmaxyx()[1] - 2)))
            cursor_y = rect.y + 3
            cursor_x = rect.x + 2 + min(cursor, max(0, rect.width - 5))
            try:
                screen.move(cursor_y, cursor_x)
                screen.refresh()
            except curses.error:
                pass
            key = screen.get_wch()
            if key in ("\n", "\r"):
                result = "".join(value)
                return result if result else None
            if key == "\x1b":
                return None
            if key in (curses.KEY_LEFT,):
                cursor = max(0, cursor - 1)
            elif key in (curses.KEY_RIGHT,):
                cursor = min(len(value), cursor + 1)
            elif key in (curses.KEY_HOME,):
                cursor = 0
            elif key in (curses.KEY_END,):
                cursor = len(value)
            elif key in (curses.KEY_BACKSPACE, "\b", "\x7f"):
                if cursor:
                    del value[cursor - 1]
                    cursor -= 1
            elif key == curses.KEY_DC:
                if cursor < len(value):
                    del value[cursor]
            elif isinstance(key, str) and key.isprintable() and len(value) < 4096:
                value.insert(cursor, key)
                cursor += 1
    finally:
        try:
            curses.curs_set(0)
        except curses.error:
            pass


def confirm_dialog(screen: Any, title: str, message: str,
                   manager: ThemeManager) -> bool:
    draw_overlay(screen, title, [message, "", "y overwrite   N cancel"], manager, 58)
    while True:
        key = screen.getch()
        if key in (ord("y"), ord("Y")):
            return True
        if key in (ord("n"), ord("N"), 27, 10, 13):
            return False


HELP_LINES = (
    "1 status only   2 stream only   3 split",
    "Tab change focus   r reverse   l cycle layout",
    "p pause/resume viewport   G/End return live",
    "Up/Down/PgUp/PgDn/Home browse rows",
    "Space toggle selection   a select visible   c clear",
    "Enter inspect raw JSON   e export selected JSON",
    "f category checklist   o runtime options   t theme",
    "F5 refresh now   q quit",
)


def handle_key(screen: Any, key: int, state: DashboardState,
               manager: ThemeManager, workers: WorkerState,
               exporter: Optional[ExportWorker] = None) -> Tuple[bool, bool]:
    """Handle one key; return (continue, theme_changed)."""
    if key in (ord("q"), 3):
        return False, False
    if key in (ord("?"), curses.KEY_F1):
        modal_message(screen, "HELP", HELP_LINES, manager)
    elif key in (9,):
        state.focus = "status" if state.focus == "events" else "events"
    elif key == ord("1"):
        state.view, state.focus = "status", "status"
    elif key == ord("2"):
        state.view, state.focus = "events", "events"
    elif key == ord("3"):
        state.view = "both"
    elif key == ord("r"):
        state.reverse = not state.reverse
    elif key == ord("l"):
        choices = ("auto", "horizontal", "vertical")
        state.layout = choices[(choices.index(state.layout) + 1) % len(choices)]
    elif key == ord("p"):
        state.paused = not state.paused
        if state.paused:
            state.history.follow = False
        else:
            state.history.end(state.event_filter)
            state.unseen = 0
    elif key == ord("f"):
        modal_filter(screen, state, manager)
    elif key == ord("o"):
        old_theme = state.theme
        modal_options(screen, state, manager)
        workers.wake_status.set()
        return True, state.theme != old_theme
    elif key == ord("t"):
        names = tuple(THEMES)
        current = names.index(state.theme) if state.theme in names else 0
        state.theme = names[(current + 1) % len(names)]
        return True, True
    elif key == curses.KEY_F5:
        workers.wake_status.set()
    elif state.focus == "status" and key in (curses.KEY_UP, ord("k")):
        state.status_scroll = max(0, state.status_scroll - 1)
    elif state.focus == "status" and key in (curses.KEY_DOWN, ord("j")):
        state.status_scroll += 1
    elif state.focus == "events":
        if key in (curses.KEY_UP, ord("k")):
            state.history.move(-1, state.event_filter)
            state.paused = True
        elif key in (curses.KEY_DOWN, ord("j")):
            state.history.move(1, state.event_filter)
            state.paused = not state.history.follow
            if not state.paused:
                state.unseen = 0
        elif key == curses.KEY_PPAGE:
            state.history.move(-10, state.event_filter)
            state.paused = True
        elif key == curses.KEY_NPAGE:
            state.history.move(10, state.event_filter)
            state.paused = not state.history.follow
            if not state.paused:
                state.unseen = 0
        elif key in (curses.KEY_HOME, ord("g")):
            state.history.home(state.event_filter)
            state.paused = True
        elif key in (curses.KEY_END, ord("G")):
            state.history.end(state.event_filter)
            state.paused = False
            state.unseen = 0
        elif key in (ord(" "), ord("x")):
            try:
                state.history.toggle(state.event_filter)
            except ValueError as error:
                state.notify(str(error))
        elif key == ord("a"):
            rows, columns = screen.getmaxyx()
            layout = resolve_layout(state.view, state.layout, state.reverse, rows, columns)
            height = max(0, (layout.events.height - 2) if layout.events else rows - 2)
            state.history.select_sequences(item.sequence for item in
                state.history.visible(height, state.event_filter))
        elif key == ord("c"):
            state.history.clear_selection()
            state.notify("Selection cleared")
        elif key in (10, 13, curses.KEY_ENTER):
            current = state.history.current(state.event_filter)
            if current:
                raw_lines = json.dumps(current.event, ensure_ascii=False,
                                       indent=2).splitlines()
                modal_message(screen, "EVENT JSON", raw_lines, manager)
        elif key == ord("e"):
            if state.export_in_progress:
                state.notify("An export is already in progress")
                return True, False
            selected = state.history.selected_events()
            if not selected:
                state.notify("Select one or more event rows first")
            else:
                filename = text_dialog(screen, f"EXPORT {len(selected)} EVENTS",
                                       "Filename:", manager,
                                       "mss-selected-events.json")
                if filename:
                    destination = Path(filename).expanduser()
                    if destination.exists() and not confirm_dialog(
                            screen, "OVERWRITE?", str(destination), manager):
                        state.notify("Export cancelled")
                    else:
                        if exporter is not None:
                            if exporter.submit(destination, selected):
                                state.export_in_progress = True
                                state.notify(f"Exporting {len(selected)} events…", 60)
                            else:
                                state.notify("An export is already queued")
                        else:
                            # Pure-controller tests and non-curses callers can
                            # still use the synchronous primitive directly.
                            try:
                                export_selected_events(destination, selected)
                                state.notify(f"Exported {len(selected)} events to {destination}", 6)
                            except Exception as error:
                                state.notify(f"Export failed: {error}", 8)
    return True, False


def run_curses(screen: Any, args: argparse.Namespace) -> int:
    try:
        curses.curs_set(0)
    except curses.error:
        pass
    screen.keypad(True)
    screen.timeout(100)
    try:
        curses.start_color()
    except curses.error:
        pass

    initial_theme = args.theme
    if initial_theme == "auto":
        initial_theme = ("tokyo-neon" if args.ui != "tty" and
                         getattr(curses, "COLORS", 0) >= 256 else "black")
    state = DashboardState(
        view=args.view,
        layout=args.layout,
        reverse=args.reverse,
        theme=initial_theme,
        interval=args.interval,
        event_rate=args.event_rate,
        min_share_difficulty=args.min_share_difficulty,
        event_filter=EventFilter(args.stream_filter),
        history=EventBuffer(args.history_size),
        focus="events" if args.view != "status" else "status",
    )
    manager = ThemeManager(screen, state.theme)
    workers = WorkerState()
    collector = StatusCollector(args.api_url, args.api_token,
                                args.monero_rpc_url, Path(args.disk_path))
    sampler = AdaptiveSampler(args.event_rate, args.min_share_difficulty)
    exporter = ExportWorker()
    status_thread = threading.Thread(
        target=status_worker,
        args=(workers, collector, lambda: state.interval),
        name="mss-status", daemon=True,
    )
    event_thread = threading.Thread(
        target=event_worker,
        args=(workers, Path(args.event_log), args.from_start, sampler,
              lambda: state.event_filter,
              lambda: (state.event_rate, state.min_share_difficulty)),
        name="mss-events", daemon=True,
    )
    status_thread.start()
    event_thread.start()
    running = True
    try:
        while running:
            while True:
                try:
                    result = exporter.results.get_nowait()
                except queue.Empty:
                    break
                state.export_in_progress = False
                if result.error:
                    state.notify(f"Export failed: {result.error}", 8)
                else:
                    state.notify(
                        f"Exported {result.count} events to {result.path}", 6)
            while True:
                try:
                    state.latest_status = workers.status_queue.get_nowait()
                except queue.Empty:
                    break
            while True:
                try:
                    entry = workers.event_queue.get_nowait()
                except queue.Empty:
                    break
                state.history.append(entry)
                if state.paused:
                    state.unseen += 1
            draw_frame(screen, state, manager, workers.event_error)
            key = screen.getch()
            if key == -1:
                continue
            running, theme_changed = handle_key(
                screen, key, state, manager, workers, exporter)
            if theme_changed:
                manager = ThemeManager(screen, state.theme)
    finally:
        workers.stop.set()
        workers.wake_status.set()
        event_thread.join(timeout=1.0)
        status_thread.join(timeout=0.2)
        exporter.close()
    return 0


def read_once_events(args: argparse.Namespace) -> List[EventEntry]:
    follower = JsonlFollower(Path(args.event_log), from_start=args.from_start)
    event_filter = EventFilter(args.stream_filter)
    sampler = AdaptiveSampler(args.event_rate, args.min_share_difficulty,
                              random.Random(0))
    selected: List[EventEntry] = []
    sequence = 0
    idle = 0
    try:
        while idle < 2:
            records = follower.poll()
            if not records:
                idle += 1
                continue
            idle = 0
            for raw in records:
                event = parse_json_event(raw)
                if event is None:
                    continue
                sequence += 1
                entry = EventEntry(sequence, event)
                if not event_filter.matches(entry):
                    continue
                disposition = sampler.add(entry)
                if disposition == "priority":
                    selected.append(entry)
        selected.extend(sampler.flush(1.0))
    finally:
        follower.close()
    selected.sort(key=lambda item: item.sequence)
    return selected


def run_plain(args: argparse.Namespace) -> int:
    color = plain_color_enabled(args)
    theme = args.theme if args.theme != "auto" else "black"
    if args.view in {"status", "both"}:
        try:
            collector = StatusCollector(args.api_url, args.api_token,
                                        args.monero_rpc_url, Path(args.disk_path))
            snapshot = collector.collect()
            for line in snapshot_status_lines(snapshot):
                print(plain_status_line(line, snapshot, theme, color))
        except Exception as error:
            print(f"mss-watch-status-tui: status collection failed: {error}",
                  file=sys.stderr)
            if args.view == "status" and args.once:
                return 1
    if args.view in {"events", "both"}:
        if args.once:
            for entry in read_once_events(args):
                print(plain_event_line(entry, theme, color))
            return 0
        follower = JsonlFollower(Path(args.event_log), from_start=args.from_start)
        event_filter = EventFilter(args.stream_filter)
        sampler = AdaptiveSampler(args.event_rate, args.min_share_difficulty)
        sequence = 0
        last_flush = time.monotonic()
        try:
            while True:
                records = follower.poll()
                for raw in records:
                    event = parse_json_event(raw)
                    if event is None:
                        continue
                    sequence += 1
                    entry = EventEntry(sequence, event)
                    if not event_filter.matches(entry):
                        continue
                    disposition = sampler.add(entry)
                    if disposition == "priority":
                        print(plain_event_line(entry, theme, color), flush=True)
                now = time.monotonic()
                if now - last_flush >= SAMPLING_WINDOW:
                    for entry in sampler.flush(now - last_flush):
                        print(plain_event_line(entry, theme, color), flush=True)
                    last_flush = now
                time.sleep(0.05)
        except KeyboardInterrupt:
            return 0
        except BrokenPipeError:
            return 0
        finally:
            follower.close()
    return 0


def positive_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be a number") from error
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return value


def bounded_positive_int(text: str) -> int:
    try:
        value = int(text, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if not 1 <= value <= MAX_HISTORY_SIZE:
        raise argparse.ArgumentTypeError(
            f"must be from 1 through {MAX_HISTORY_SIZE}")
    return value


def positive_difficulty(text: str) -> int:
    try:
        value = int(text, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if not 1 <= value <= (1 << 64) - 1:
        raise argparse.ArgumentTypeError("must be from 1 through 18446744073709551615")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Split-screen status and adaptive event monitor for monero-solo-stratum.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=("Interactive filter: f opens category checkboxes; Space toggles, "
                "A selects all, N selects none.\n"
                "Important events bypass rate sampling only when their category is enabled."),
    )
    parser.add_argument("--api-url", default=DEFAULT_API_URL,
                        help=f"pool API base URL (default {DEFAULT_API_URL})")
    parser.add_argument("--api-token", default="", help="optional exact API Bearer token")
    parser.add_argument("--monero-rpc-url", "--monerod-url",
                        default=DEFAULT_MONERO_RPC_URL,
                        help=f"monerod RPC base URL (default {DEFAULT_MONERO_RPC_URL})")
    parser.add_argument("--interval", type=positive_float, default=5.0,
                        help="status refresh seconds (default 5)")
    parser.add_argument("--event-log", "--event-file", default=DEFAULT_EVENT_LOG,
                        help=f"JSONL log to follow (default {DEFAULT_EVENT_LOG})")
    parser.add_argument("--event-rate", type=positive_float,
                        default=DEFAULT_EVENT_RATE,
                        help="target total ordinary stream rows/second (default 2)")
    parser.add_argument("--min-share-difficulty", type=positive_difficulty,
                        default=DEFAULT_MIN_SHARE_DIFFICULTY,
                        help="base accepted-share display floor (default 100000000)")
    parser.add_argument("--stream-filter", default="",
                        help="comma-separated category checklist selection (default all)")
    parser.add_argument("--view", choices=("both", "status", "events"),
                        default="both", help="initial pane view (default both)")
    parser.add_argument("--layout", choices=("auto", "horizontal", "vertical"),
                        default="auto", help="pane tiling (default auto)")
    parser.add_argument("--reverse", action="store_true", help="reverse pane order")
    parser.add_argument("--ui", choices=("auto", "curses", "tty", "plain"),
                        default="auto", help="renderer/capability family (default auto)")
    parser.add_argument("--theme", choices=("auto", *THEMES.keys()), default="auto",
                        help="initial color theme (default auto)")
    parser.add_argument("--from-start", action="store_true",
                        help="read existing event records instead of starting at EOF")
    parser.add_argument("--disk-path", default="/var/lib/monero-solo-stratum",
                        help="filesystem path used for disk capacity/I/O (default state directory)")
    parser.add_argument("--history-size", type=bounded_positive_int,
                        default=DEFAULT_HISTORY_SIZE,
                        help=f"bounded retained event rows (default {DEFAULT_HISTORY_SIZE})")
    parser.add_argument("--once", action="store_true",
                        help="render one finite plain snapshot and exit")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        EventFilter(args.stream_filter)
        HttpJsonClient(args.api_url, args.api_token)
        HttpJsonClient(args.monero_rpc_url)
    except ValueError as error:
        parser.error(str(error))
    use_curses = (not args.once and args.ui in {"auto", "curses"} and
                  sys.stdin.isatty() and sys.stdout.isatty() and
                  os.environ.get("TERM", "dumb") != "dumb")
    if args.ui == "curses" and not use_curses:
        parser.error("--ui curses requires an interactive terminal")
    if not use_curses:
        return run_plain(args)
    try:
        return curses.wrapper(lambda screen: run_curses(screen, args))
    except KeyboardInterrupt:
        return 0
    except curses.error as error:
        print(f"mss-watch-status-tui: terminal initialization failed: {error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
