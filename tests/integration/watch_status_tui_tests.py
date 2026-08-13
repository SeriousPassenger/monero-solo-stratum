#!/usr/bin/env python3
"""Tests for the standard-library split-screen status/event monitor."""

from __future__ import annotations

import importlib.util
import io
import json
import os
import pty
import random
import select
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import ModuleType
from typing import Any, Dict, Iterable, List
from unittest import mock


def load_tui(path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location("watch_status_tui", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import {path}")
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves forward references through sys.modules while a
    # module is being executed.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
    TUI_PATH = Path(sys.argv.pop(1)).resolve()
else:
    TUI_PATH = (
        Path(__file__).resolve().parents[2] / "scripts" / "watch-status-tui.py"
    )

tui = load_tui(TUI_PATH)


def event(
    sequence: int,
    code: str,
    *,
    severity: str = "info",
    status: str | None = None,
    difficulty: int | None = None,
    **fields: Any,
) -> Dict[str, Any]:
    body: Dict[str, Any] = {
        "time": f"2026-08-13T12:00:{sequence % 60:02d}.000000Z",
        "severity": severity,
        "code": code,
        "fields": dict(fields),
    }
    if status is not None:
        body["fields"]["status"] = status
    if difficulty is not None:
        body["fields"]["actual_difficulty"] = str(difficulty)
    return body


def entry(sequence: int, body: Dict[str, Any]) -> Any:
    """Build EventEntry without depending on optional presentation fields."""
    cls = tui.EventEntry
    attempts = (
        {"sequence": sequence, "event": body},
        {"seq": sequence, "event": body},
        {"sequence": sequence, "data": body},
        {"seq": sequence, "data": body},
        {"sequence": sequence, "raw": body},
    )
    for values in attempts:
        try:
            return cls(**values)
        except TypeError:
            pass
    try:
        return cls(sequence, body)
    except TypeError as error:
        raise AssertionError(f"unsupported EventEntry constructor: {error}") from error


def event_data(item: Any) -> Dict[str, Any]:
    for name in ("event", "data", "raw"):
        value = getattr(item, name, None)
        if isinstance(value, dict):
            return value
    if isinstance(item, dict):
        return item
    raise AssertionError(f"cannot extract event dictionary from {item!r}")


def sampler(rate: float, minimum: int = 100_000_000) -> Any:
    attempts = (
        {"rate": rate, "min_share_difficulty": minimum, "rng": random.Random(7)},
        {"rows_per_second": rate, "min_share_difficulty": minimum,
         "rng": random.Random(7)},
        {"rate": rate, "min_share_difficulty": minimum},
        {"rows_per_second": rate, "min_share_difficulty": minimum},
    )
    for values in attempts:
        try:
            result = tui.AdaptiveSampler(**values)
            if hasattr(result, "rng"):
                result.rng = random.Random(7)
            return result
        except TypeError:
            pass
    raise AssertionError("unsupported AdaptiveSampler constructor")


class PruneAndExportTests(unittest.TestCase):
    def test_deep_prune_empty_keeps_false_and_zero(self) -> None:
        source = {
            "empty": "",
            "none": None,
            "empty_list": [],
            "nested": {"drop": None, "keep": 0},
            "list": [None, "", {"gone": [], "yes": False}, 7],
            "false": False,
            "zero": 0,
        }
        self.assertEqual(
            tui.deep_prune_empty(source),
            {
                "nested": {"keep": 0},
                "list": [{"yes": False}, 7],
                "false": False,
                "zero": 0,
            },
        )

    def test_export_is_json_mode_0600_and_omits_empty_fields(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "chosen-events.json"
            records = [
                event(1, "share.completed", status="accepted", difficulty=500_000_000,
                      optional=None, label=""),
                event(2, "template.refreshed", generation=8, unused=[]),
            ]
            tui.export_selected_events(destination, records)
            parsed = json.loads(destination.read_text(encoding="utf-8"))
            self.assertIsInstance(parsed, list)
            self.assertEqual(len(parsed), 2)
            self.assertNotIn("optional", parsed[0]["fields"])
            self.assertNotIn("label", parsed[0]["fields"])
            self.assertNotIn("unused", parsed[1]["fields"])
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)
            self.assertEqual(list(destination.parent.glob(".*.tmp")), [])

    def test_failed_export_does_not_damage_existing_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "existing.json"
            destination.write_text('[{"old":true}]\n', encoding="utf-8")
            original = destination.read_bytes()
            unserializable = event(1, "job.queued", unsupported=object())
            with self.assertRaises((TypeError, ValueError)):
                tui.export_selected_events(destination, [unserializable])
            self.assertEqual(destination.read_bytes(), original)

    def test_export_accepts_event_entries(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "entries.json"
            item = entry(4, event(4, "job.queued", height=33))
            tui.export_selected_events(destination, [item])
            parsed = json.loads(destination.read_text(encoding="utf-8"))
            self.assertEqual(parsed[0]["code"], "job.queued")

    def test_export_rejects_symlink_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            victim = root / "victim.json"
            victim.write_text("do not overwrite", encoding="utf-8")
            link = root / "events.json"
            link.symlink_to(victim)
            with self.assertRaises((OSError, ValueError)):
                tui.export_selected_events(link, [event(1, "runtime.ready")])
            self.assertEqual(victim.read_text(encoding="utf-8"), "do not overwrite")

    def test_async_export_worker_reports_completion_and_closes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "async-selection.json"
            records = [
                entry(1, event(1, "runtime.ready", empty=None)),
                entry(2, event(2, "job.queued", job_id=9, unused="")),
            ]
            worker = tui.ExportWorker()
            try:
                self.assertTrue(worker.submit(destination, records))
                result = worker.results.get(timeout=5.0)
            finally:
                worker.close()

            self.assertIsNone(result.error)
            self.assertEqual(result.path, destination)
            self.assertEqual(result.count, 2)
            self.assertFalse(worker.thread.is_alive())
            exported = json.loads(destination.read_text(encoding="utf-8"))
            self.assertEqual([item["code"] for item in exported],
                             ["runtime.ready", "job.queued"])
            self.assertNotIn("fields", exported[0])
            self.assertNotIn("unused", exported[1]["fields"])
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o600)


class ThemeAndLayoutTests(unittest.TestCase):
    def test_all_five_documented_themes_exist(self) -> None:
        expected = {
            "nerv-asuka",
            "tokyo-neon",
            "windows-classic",
            "black",
            "windows-classic-tty",
        }
        self.assertTrue(expected.issubset(set(tui.THEMES)))

    def test_theme_capability_fallbacks_are_usable(self) -> None:
        for name in tui.THEMES:
            monochrome = tui.select_theme(name, 0)
            basic = tui.select_theme(name, 8)
            rich = tui.select_theme(name, 256)
            self.assertIsNotNone(monochrome)
            self.assertIsNotNone(basic)
            self.assertIsNotNone(rich)

    def test_resolve_layout_modes_views_and_reverse(self) -> None:
        status_only = tui.resolve_layout("status", "auto", False, 30, 120)
        events_only = tui.resolve_layout("events", "auto", False, 30, 120)
        wide = tui.resolve_layout("both", "auto", False, 30, 120)
        narrow = tui.resolve_layout("both", "auto", False, 40, 70)
        reversed_wide = tui.resolve_layout("both", "horizontal", True, 30, 120)
        explicit_vertical = tui.resolve_layout("both", "vertical", False, 30, 120)
        explicit_horizontal = tui.resolve_layout("both", "horizontal", False, 30, 120)

        self.assertNotEqual(status_only, events_only)
        self.assertNotEqual(wide, narrow)
        self.assertNotEqual(explicit_vertical, explicit_horizontal)
        self.assertIn(wide, (explicit_vertical, explicit_horizontal))
        narrow_vertical = tui.resolve_layout("both", "vertical", False, 40, 70)
        narrow_horizontal = tui.resolve_layout("both", "horizontal", False, 40, 70)
        self.assertIn(narrow, (narrow_vertical, narrow_horizontal))
        self.assertNotEqual(explicit_horizontal, reversed_wide)

    def test_resolve_layout_is_bounded_on_tiny_terminal(self) -> None:
        value = tui.resolve_layout("both", "auto", False, 2, 3)
        numbers: List[int] = []

        def collect(part: Any) -> None:
            if isinstance(part, bool):
                return
            if isinstance(part, int):
                numbers.append(part)
            elif isinstance(part, dict):
                for nested in part.values():
                    collect(nested)
            elif isinstance(part, (tuple, list)):
                for nested in part:
                    collect(nested)
            elif hasattr(part, "__dict__"):
                collect(vars(part))

        collect(value)
        self.assertTrue(numbers)
        self.assertTrue(all(number >= 0 for number in numbers))


class EventBufferAndFilterTests(unittest.TestCase):
    def test_category_checklist_space_all_none_apply_and_cancel(self) -> None:
        keys = set(tui.FILTER_KEYS)
        checklist = tui.CategoryChecklist(keys)
        first = tui.FILTER_GROUPS[0][0]

        self.assertIsNone(checklist.handle(" "))
        self.assertNotIn(first, checklist.enabled)
        self.assertIsNone(checklist.handle("N"))
        self.assertEqual(checklist.enabled, set())
        self.assertIsNone(checklist.handle("A"))
        self.assertEqual(checklist.enabled, keys)
        checklist.handle(" ")
        applied = set(checklist.enabled)
        self.assertEqual(checklist.handle(10), "apply")
        self.assertEqual(checklist.enabled, applied)

        cancelled = tui.CategoryChecklist(keys)
        cancelled.handle("N")
        self.assertEqual(cancelled.enabled, set())
        self.assertEqual(cancelled.handle(27), "cancel")
        self.assertEqual(cancelled.enabled, keys)

    def test_category_checklist_supports_one_many_and_all(self) -> None:
        all_groups = set(tui.FILTER_KEYS)
        checklist = tui.CategoryChecklist([])
        checklist.handle(" ")
        self.assertEqual(len(checklist.enabled), 1)
        checklist.handle("j")
        checklist.handle(" ")
        self.assertEqual(len(checklist.enabled), 2)
        checklist.handle("a")
        self.assertEqual(checklist.enabled, all_groups)

    def test_category_checklist_none_then_apply_remains_none(self) -> None:
        checklist = tui.CategoryChecklist(tui.FILTER_KEYS)
        self.assertIsNone(checklist.handle("N"))
        self.assertEqual(checklist.handle(10), "apply")
        event_filter = tui.EventFilter(checklist.enabled)
        self.assertEqual(event_filter.enabled, set())
        self.assertFalse(event_filter.matches(
            entry(1, event(1, "candidate.accepted", severity="critical"))
        ))

    def test_event_buffer_is_bounded_and_selection_survives_navigation(self) -> None:
        buffer = tui.EventBuffer(3)
        items = [entry(index, event(index, "job.queued", job_id=index))
                 for index in range(1, 6)]
        for item in items:
            buffer.append(item)
        self.assertLessEqual(len(buffer), 3)

        current = buffer.current()
        self.assertIsNotNone(current)
        buffer.toggle()
        selected = buffer.selected_events()
        self.assertEqual(len(selected), 1)
        self.assertEqual(event_data(selected[0])["fields"]["job_id"],
                         event_data(current)["fields"]["job_id"])

    def test_event_buffer_drops_evicted_selections(self) -> None:
        buffer = tui.EventBuffer(2)
        first = entry(1, event(1, "job.queued"))
        buffer.append(first)
        buffer.toggle()
        buffer.append(entry(2, event(2, "job.queued")))
        buffer.append(entry(3, event(3, "job.queued")))
        self.assertEqual(buffer.selected_events(), [])

    def test_event_filter_can_match_one_or_more_streams(self) -> None:
        all_filter = tui.EventFilter("")
        shares_jobs = tui.EventFilter("share,job")
        share = entry(1, event(1, "share.completed", status="accepted",
                               difficulty=200_000_000))
        job = entry(2, event(2, "job.queued"))
        template = entry(3, event(3, "template.refreshed"))

        def matches(filter_object: Any, item: Any) -> bool:
            for name in ("matches", "match", "allows", "accepts"):
                method = getattr(filter_object, name, None)
                if method is not None:
                    return bool(method(item))
            if callable(filter_object):
                return bool(filter_object(item))
            self.fail("EventFilter has no match method")

        self.assertTrue(matches(all_filter, template))
        self.assertTrue(matches(shares_jobs, share))
        self.assertTrue(matches(shares_jobs, job))
        self.assertFalse(matches(shares_jobs, template))

    def test_disabled_category_hides_priority_rows_too(self) -> None:
        templates_only = tui.EventFilter("template")
        duplicate = entry(1, event(1, "share.completed", status="duplicate",
                                   difficulty=500_000_000))
        fatal_connection = entry(2, event(2, "connection.failed",
                                          severity="fatal"))
        refreshed = entry(3, event(3, "template.refreshed"))

        def matches(item: Any) -> bool:
            for name in ("matches", "match", "allows", "accepts"):
                method = getattr(templates_only, name, None)
                if method is not None:
                    return bool(method(item))
            if callable(templates_only):
                return bool(templates_only(item))
            self.fail("EventFilter has no match method")

        # Filtering is a user choice made before adaptive sampling. Priority
        # is not permission to cross a disabled category boundary.
        self.assertFalse(matches(duplicate))
        self.assertFalse(matches(fatal_connection))
        self.assertTrue(matches(refreshed))

    def test_filter_relocates_cursor_before_selection(self) -> None:
        buffer = tui.EventBuffer(10)
        visible_job = entry(1, event(1, "job.queued", job_id=1))
        hidden_template = entry(2, event(2, "template.refreshed", generation=2))
        buffer.append(visible_job)
        buffer.append(hidden_template)
        self.assertEqual(buffer.current().sequence, 2)

        rows = buffer.visible(10, tui.EventFilter("jobs"))
        self.assertEqual([item.sequence for item in rows], [1])
        self.assertEqual(buffer.current().sequence, 1)
        buffer.toggle()
        self.assertEqual(
            [item.sequence for item in buffer.selected_events()],
            [1],
        )


class AdaptiveSamplerTests(unittest.TestCase):
    def test_received_half_of_share_pair_is_dropped(self) -> None:
        sample = sampler(2)
        result = sample.add(entry(1, event(1, "share.received",
                                                difficulty=9_000_000_000)))
        self.assertEqual(result, "drop")

    def test_low_shares_drop_and_high_shares_are_adaptively_ranked(self) -> None:
        sample = sampler(2)
        low = sample.add(entry(1, event(1, "share.completed", status="accepted",
                                            difficulty=99_999_999)))
        self.assertEqual(low, "drop")
        difficulties = [100_000_000, 200_000_000, 9_000_000_000, 800_000_000]
        for index, difficulty in enumerate(difficulties, 2):
            self.assertEqual(
                sample.add(entry(index, event(index, "share.completed",
                                               status="accepted",
                                               difficulty=difficulty))),
                "sampled",
            )
        shown = sample.flush(1.0)
        actual = [int(event_data(item)["fields"]["actual_difficulty"])
                  for item in shown]
        self.assertIn(9_000_000_000, actual)
        self.assertLessEqual(len(shown), 2)

    def test_priority_events_bypass_sampling(self) -> None:
        sample = sampler(0.25)
        priority = [
            event(1, "candidate.accepted"),
            event(2, "blocknotify.failed"),
            event(3, "anything", severity="error"),
            event(4, "share.completed", status="duplicate", difficulty=300_000_000),
        ]
        for index, body in enumerate(priority, 1):
            self.assertEqual(sample.add(entry(index, body)), "priority")

    def test_global_rate_applies_across_noisy_event_classes(self) -> None:
        sample = sampler(4)
        sequence = 0
        shown: List[Any] = []
        for second in range(10):
            for index in range(100):
                sequence += 1
                code = ("job.queued", "template.refreshed",
                        "connection.opened")[index % 3]
                sample.add(entry(sequence, event(sequence, code, job_id=index)))
            for index in range(20):
                sequence += 1
                sample.add(entry(sequence, event(
                    sequence, "share.completed", status="accepted",
                    difficulty=100_000_000 + index * 1_000_000,
                )))
            shown.extend(sample.flush(1.0))
        self.assertLessEqual(len(shown), 40)
        # A continuously noisy non-share class must not force the sample to be
        # shares-only, and accepted shares must not disappear either.
        codes = {event_data(item)["code"] for item in shown}
        self.assertIn("share.completed", codes)
        self.assertTrue(codes & {"job.queued", "template.refreshed",
                                 "connection.opened"})

    def test_fractional_rate_accumulates_credit(self) -> None:
        sample = sampler(0.25)
        shown: List[Any] = []
        for index in range(8):
            sample.add(entry(index, event(index, "job.queued")))
            shown.extend(sample.flush(1.0))
        self.assertEqual(len(shown), 2)

    def test_template_flood_is_sampled_without_starving_other_streams(self) -> None:
        sample = sampler(2)
        shown: List[Any] = []
        sequence = 0
        for _second in range(12):
            for generation in range(200):
                sequence += 1
                sample.add(entry(sequence, event(
                    sequence, "template.refreshed", generation=generation,
                )))
            sequence += 1
            sample.add(entry(sequence, event(sequence, "job.queued", job_id=sequence)))
            shown.extend(sample.flush(1.0))
        codes = [event_data(item)["code"] for item in shown]
        self.assertLessEqual(len(codes), 24)
        self.assertIn("template.refreshed", codes)
        self.assertIn("job.queued", codes)
        sampled_generations = [
            event_data(item)["fields"]["generation"]
            for item in shown
            if event_data(item)["code"] == "template.refreshed"
        ]
        # Reservoir selection must not deterministically print only the first
        # record of every flood window.
        self.assertTrue(any(value > 10 for value in sampled_generations))

    def test_internal_candidates_remain_bounded_during_a_flood(self) -> None:
        sample = sampler(1)
        for index in range(50_000):
            sample.add(entry(index, event(index, "job.queued", job_id=index)))
        containers: List[int] = []
        for value in vars(sample).values():
            if isinstance(value, (list, tuple, set, dict)):
                containers.append(len(value))
            elif hasattr(value, "__len__") and value.__class__.__module__ == tui.__name__:
                try:
                    containers.append(len(value))
                except TypeError:
                    pass
        self.assertTrue(containers)
        self.assertLess(max(containers), 10_000)

    def test_runtime_rate_increase_resizes_sampling_capacity(self) -> None:
        sample = sampler(1)
        # Runtime options update this same property while the worker remains
        # alive. Raising the target must not leave the initial eight-record
        # reservoir as a hidden ceiling.
        sample.rate = 100.0
        for index in range(200):
            sample.add(entry(index, event(index, "job.queued", job_id=index)))
        shown = sample.flush(1.0)
        self.assertEqual(len(shown), 100)


class SystemMetricTests(unittest.TestCase):
    def test_parse_proc_stat(self) -> None:
        parsed = tui.parse_proc_stat("cpu  10 2 3 80 5 1 2 0 0 0\n")
        if isinstance(parsed, dict):
            total = parsed.get("total")
            idle = parsed.get("idle")
        else:
            total, idle = parsed[:2]
        self.assertEqual(total, 103)
        self.assertEqual(idle, 85)

    def test_parse_meminfo(self) -> None:
        parsed = tui.parse_meminfo(
            "MemTotal:       65536000 kB\n"
            "MemFree:         1000000 kB\n"
            "MemAvailable:   50000000 kB\n"
            "Buffers:          200000 kB\n"
        )
        if isinstance(parsed, dict):
            total = parsed.get("total", parsed.get("MemTotal"))
            available = parsed.get("available", parsed.get("MemAvailable"))
        else:
            total, available = parsed[:2]
        self.assertEqual(total, 65_536_000 * 1024)
        self.assertEqual(available, 50_000_000 * 1024)

    def test_parse_net_dev_aggregates_non_loopback_interfaces(self) -> None:
        text = (
            "Inter-| Receive | Transmit\n"
            " face |bytes packets errs drop fifo frame compressed multicast|bytes packets\n"
            " lo: 100 1 0 0 0 0 0 0 200 2 0 0 0 0 0 0\n"
            " eth0: 1000 2 0 0 0 0 0 0 3000 3 0 0 0 0 0 0\n"
            " wg0: 500 2 0 0 0 0 0 0 700 3 0 0 0 0 0 0\n"
        )
        parsed = tui.parse_net_dev(text)
        if isinstance(parsed, dict) and "rx" in parsed:
            received, sent = parsed["rx"], parsed["tx"]
        elif isinstance(parsed, dict):
            received = sum(value[0] for key, value in parsed.items() if key != "lo")
            sent = sum(value[1] for key, value in parsed.items() if key != "lo")
        else:
            received, sent = parsed[:2]
        self.assertEqual(received, 1500)
        self.assertEqual(sent, 3700)

    def test_parse_diskstats_excludes_partitions_and_loop_devices(self) -> None:
        text = (
            "259 0 nvme0n1 10 0 100 0 20 0 300 0 0 400 0 0 0 0\n"
            "259 1 nvme0n1p1 99 0 900 0 99 0 900 0 0 900 0 0 0 0\n"
            "7 0 loop0 99 0 900 0 99 0 900 0 0 900 0 0 0 0\n"
            "9 0 md0 1 0 50 0 2 0 70 0 0 100 0 0 0 0\n"
        )
        read_sectors, written_sectors, busy_ms = tui.parse_diskstats(text)
        self.assertEqual((read_sectors, written_sectors, busy_ms),
                         (50, 70, 100))
        self.assertEqual(tui.parse_diskstats(text, device=(259, 0)),
                         (100, 300, 400))

    def test_host_metrics_computes_capacity_and_second_sample_rates(self) -> None:
        metric = tui.HostMetrics(Path("/var/lib/monero-solo-stratum"))
        proc_values = {
            "/proc/stat": ["cpu 10 0 10 80 0\n", "cpu 30 0 20 150 0\n"],
            "/proc/meminfo": [
                "MemTotal: 1000 kB\nMemAvailable: 750 kB\n",
                "MemTotal: 1000 kB\nMemAvailable: 700 kB\n",
            ],
            "/proc/net/dev": [
                "eth0: 100 0 0 0 0 0 0 0 200 0 0 0 0 0 0 0\n",
                "eth0: 1100 0 0 0 0 0 0 0 2200 0 0 0 0 0 0 0\n",
            ],
            "/proc/diskstats": [
                "9 0 md0 0 0 10 0 0 0 20 0 0 100 0 0\n",
                "9 0 md0 0 0 30 0 0 0 60 0 0 600 0 0\n",
            ],
        }

        def fake_read(path: str) -> str:
            values = proc_values[path]
            return values.pop(0)

        fake_statvfs = type("Vfs", (), {
            "f_blocks": 1_000, "f_bavail": 250, "f_frsize": 4096,
        })()
        with mock.patch.object(tui, "read_text", side_effect=fake_read), \
             mock.patch.object(tui.time, "monotonic", side_effect=[10.0, 12.0]), \
             mock.patch.object(tui.os, "statvfs", return_value=fake_statvfs):
            first = metric.collect()
            second = metric.collect()

        self.assertIsNone(first["cpu_percent"])
        self.assertEqual(second["memory_total"], 1_024_000)
        self.assertEqual(second["memory_available"], 716_800)
        self.assertAlmostEqual(second["network_rx_per_second"], 500.0)
        self.assertAlmostEqual(second["network_tx_per_second"], 1000.0)
        self.assertAlmostEqual(second["disk_read_per_second"], 5120.0)
        self.assertAlmostEqual(second["disk_write_per_second"], 10240.0)
        self.assertAlmostEqual(second["disk_busy_percent"], 25.0)
        self.assertEqual(second["disk_total"], 4_096_000)
        self.assertEqual(second["disk_available"], 1_024_000)


class JsonlFollowerTests(unittest.TestCase):
    def test_follows_rename_rotation_from_start_of_replacement(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "debug.jsonl"
            path.write_bytes(b'{"code":"first"}\n')
            follower = tui.JsonlFollower(path, from_start=True)
            try:
                self.assertEqual(follower.poll(), [b'{"code":"first"}'])
                rotated = path.with_suffix(".jsonl.1")
                path.rename(rotated)
                path.write_bytes(b'{"code":"second"}\n')
                self.assertEqual(follower.poll(), [])  # notices inode change
                self.assertEqual(follower.poll(), [b'{"code":"second"}'])
            finally:
                follower.close()

    def test_recovers_after_copytruncate(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "debug.jsonl"
            path.write_bytes(b'{"code":"a-very-long-original-record"}\n')
            follower = tui.JsonlFollower(path, from_start=True)
            try:
                self.assertEqual(len(follower.poll()), 1)
                path.write_bytes(b'{"code":"new"}\n')
                self.assertEqual(follower.poll(), [])  # notices smaller size
                self.assertEqual(follower.poll(), [b'{"code":"new"}'])
            finally:
                follower.close()

    def test_partial_line_is_held_until_newline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "debug.jsonl"
            path.write_bytes(b'{"code":"partial"}')
            follower = tui.JsonlFollower(path, from_start=True)
            try:
                self.assertEqual(follower.poll(), [])
                with path.open("ab") as stream:
                    stream.write(b"\n")
                self.assertEqual(follower.poll(), [b'{"code":"partial"}'])
            finally:
                follower.close()


class StatusFormattingTests(unittest.TestCase):
    @staticmethod
    def snapshot(**overrides: Any) -> Any:
        values: Dict[str, Any] = {
            "captured_at": "2026-08-13T12:00:00Z",
            "summary": {"data": {
                "server": {"network": "mainnet", "uptime_seconds": 3661},
                "daemon": {"height": 3_739_481, "rpc": "healthy",
                           "zmq": "healthy"},
                "connections": {"active": 45},
                "workers": {"active": 1},
                "hashrate": {"1m": "23000000", "5m": "24000000",
                             "1h": "18600000", "source": "verified"},
                "shares": {"accepted": "100", "total": "102",
                           "stale": "1", "duplicate": "1"},
                "round": {"id": "1", "state": "open",
                          "estimated_hashes": "390000000000",
                          "effort": {"value": "56.74"}},
                "candidates": {"accepted": 0, "total": 0},
            }},
            "ready": {"data": {"ready": True}},
            "verifier": {"data": {
                "enabled": True,
                "provenance": "verified",
                "configuration": {"memory_mode": "fast"},
                "stats": {"workers": 6, "active_seed_id": "fallback",
                          "seeds_ready": 1, "seeds_preparing": 0,
                          "pending": 0, "failed": 0},
                "seeds": [{"seed_id": "seed-current-123", "state": "current",
                           "all_vms_use_large_pages": True}],
            }},
            "persistence": {"data": {"database_bytes": 4096,
                                       "wal_bytes": 512,
                                       "writer_queue_items": 0}},
            "top": {"data": []},
            "high": {"data": []},
            "monerod": {"height": 3_739_481, "synchronized": True,
                        "wide_difficulty": "676192185781",
                        "incoming_connections_count": 4,
                        "outgoing_connections_count": 32},
            "host": {"cpu_percent": 25.0, "memory_total": 64 * 1024 ** 3,
                     "memory_available": 48 * 1024 ** 3,
                     "network_rx_per_second": 1000,
                     "network_tx_per_second": 2000,
                     "disk_total": 2 * 1024 ** 4,
                     "disk_available": 1024 ** 4,
                     "disk_busy_percent": 12.5,
                     "disk_read_per_second": 3000,
                     "disk_write_per_second": 4000},
            "error": None,
        }
        values.update(overrides)
        return tui.StatusSnapshot(**values)

    def test_status_maps_monerod_peers_difficulty_and_current_seed(self) -> None:
        lines = tui.snapshot_status_lines(self.snapshot())
        output = "\n".join(lines)
        self.assertIn("Peers 32 out / 4 in", output)
        self.assertIn("net 676 Gdiff", output)
        self.assertIn("seed seed-current", output)
        self.assertIn("huge pages yes", output)
        self.assertIn("pending 0", output)
        self.assertIn("failed 0", output)
        self.assertIn("CPU 25.0%", output)
        self.assertIn("RAM 25.0%", output)
        self.assertIn("DISK 50.0%", output)

    def test_false_large_pages_is_reported_as_no(self) -> None:
        verifier = {"data": {
            "enabled": True,
            "provenance": "verified",
            "configuration": {"memory_mode": "fast"},
            "stats": {"workers": 6, "seeds_ready": 1,
                      "seeds_preparing": 0, "pending": 0, "failed": 0},
            "seeds": [{"seed_id": "seed-with-small-pages", "state": "current",
                       "all_vms_use_large_pages": False}],
        }}
        output = "\n".join(tui.snapshot_status_lines(
            self.snapshot(verifier=verifier)
        ))
        self.assertIn("huge pages no", output)
        self.assertNotIn("huge pages ?", output)

    def test_unavailable_sources_degrade_to_visible_placeholders(self) -> None:
        waiting = "\n".join(tui.snapshot_status_lines(None))
        self.assertIn("Waiting", waiting)
        degraded = self.snapshot(
            monerod={"error": "connection refused"},
            host={"cpu_percent": None, "memory_total": 0,
                  "memory_available": 0, "disk_total": 0,
                  "disk_available": 0},
        )
        output = "\n".join(tui.snapshot_status_lines(degraded))
        self.assertIn("Peers ? out / ? in", output)
        self.assertIn("CPU sampling", output)
        self.assertIn("RAM unavailable", output)
        self.assertIn("DISK sampling", output)

    def test_monerod_failure_does_not_discard_pool_or_host_snapshot(self) -> None:
        class Api:
            def get(self, path: str) -> Dict[str, Any]:
                if path == "/v1/summary":
                    return {"data": {"round": {"id": "1"}}}
                return {"data": [] if "shares/" in path else {}}

        class Monero:
            def post(self, path: str, body: Dict[str, Any]) -> Dict[str, Any]:
                raise OSError("monerod unavailable")

        class Host:
            def collect(self) -> Dict[str, Any]:
                return {"cpu_percent": 1.0}

        collector = object.__new__(tui.StatusCollector)
        collector.api = Api()
        collector.monero = Monero()
        collector.host = Host()
        snapshot = collector.collect()
        self.assertIn("unavailable", snapshot.monerod["error"])
        self.assertEqual(snapshot.host["cpu_percent"], 1.0)
        self.assertEqual(snapshot.summary["data"]["round"]["id"], "1")

    def test_pool_api_failure_still_collects_monerod_and_host(self) -> None:
        class Api:
            def get(self, path: str) -> Dict[str, Any]:
                if path == "/v1/summary":
                    raise OSError("summary endpoint unavailable")
                return {"data": [] if "shares/" in path else {}}

        class Monero:
            def post(self, path: str, body: Dict[str, Any]) -> Dict[str, Any]:
                return {"result": {
                    "height": 3_739_500,
                    "incoming_connections_count": 7,
                    "outgoing_connections_count": 31,
                }}

        class Host:
            def __init__(self) -> None:
                self.calls = 0

            def collect(self) -> Dict[str, Any]:
                self.calls += 1
                return {"cpu_percent": 12.5, "memory_total": 1024}

        collector = object.__new__(tui.StatusCollector)
        collector.api = Api()
        collector.monero = Monero()
        collector.host = Host()
        snapshot = collector.collect()
        self.assertEqual(snapshot.monerod["height"], 3_739_500)
        self.assertEqual(snapshot.host["cpu_percent"], 12.5)
        self.assertEqual(collector.host.calls, 1)
        self.assertIn("summary endpoint unavailable", snapshot.error or "")

    def test_api_failure_keeps_previous_snapshot_marked_stale(self) -> None:
        first = self.snapshot()

        class Collector:
            calls = 0

            def collect(self) -> Any:
                self.calls += 1
                if self.calls == 1:
                    return first
                raise OSError("API unavailable")

        state = tui.WorkerState()
        waits = 0

        def fake_wait(_seconds: float) -> bool:
            nonlocal waits
            waits += 1
            if waits >= 2:
                state.stop.set()
            return False

        with mock.patch.object(state.wake_status, "wait", side_effect=fake_wait):
            tui.status_worker(state, Collector(), lambda: 1.0)
        retained = state.status_queue.get_nowait()
        self.assertEqual(retained.summary, first.summary)
        self.assertEqual(retained.error, "API unavailable")
        self.assertEqual(state.status_error, "API unavailable")


class ControllerInteractionTests(unittest.TestCase):
    class Screen:
        def __init__(self, keys: Iterable[int] = ()) -> None:
            self.keys = list(keys)

        def getmaxyx(self) -> tuple[int, int]:
            return 30, 120

        def getch(self) -> int:
            if not self.keys:
                raise AssertionError("controller requested an unexpected key")
            return self.keys.pop(0)

        def erase(self) -> None:
            pass

        def addstr(self, *_arguments: Any) -> None:
            pass

        def refresh(self) -> None:
            pass

    class Manager:
        theme = tui.THEMES["black"]

        @staticmethod
        def attr(_role: str) -> int:
            return 0

    @staticmethod
    def state() -> Any:
        history = tui.EventBuffer(20)
        history.append(entry(1, event(1, "job.queued", job_id=1)))
        history.append(entry(2, event(2, "template.refreshed", generation=2)))
        return tui.DashboardState(
            view="both", layout="auto", reverse=False, theme="black",
            interval=5.0, event_rate=2.0,
            min_share_difficulty=100_000_000,
            event_filter=tui.EventFilter(""), history=history,
        )

    def test_view_reverse_pause_and_selection_keys(self) -> None:
        screen = self.Screen()
        manager = self.Manager()
        workers = tui.WorkerState()
        state = self.state()

        tui.handle_key(screen, ord("1"), state, manager, workers)
        self.assertEqual((state.view, state.focus), ("status", "status"))
        tui.handle_key(screen, ord("2"), state, manager, workers)
        self.assertEqual((state.view, state.focus), ("events", "events"))
        tui.handle_key(screen, ord("3"), state, manager, workers)
        self.assertEqual(state.view, "both")
        tui.handle_key(screen, ord("r"), state, manager, workers)
        self.assertTrue(state.reverse)
        tui.handle_key(screen, ord("p"), state, manager, workers)
        self.assertTrue(state.paused)
        self.assertFalse(state.history.follow)
        tui.handle_key(screen, ord("p"), state, manager, workers)
        self.assertFalse(state.paused)
        self.assertTrue(state.history.follow)

        state.focus = "events"
        tui.handle_key(screen, ord(" "), state, manager, workers)
        self.assertEqual(len(state.history.selected_events()), 1)
        tui.handle_key(screen, ord(" "), state, manager, workers)
        self.assertEqual(state.history.selected_events(), [])

    def test_navigation_back_to_live_clears_unseen_count(self) -> None:
        state = self.state()
        state.focus = "events"
        state.history.home()
        state.paused = True
        state.unseen = 9
        tui.handle_key(
            self.Screen(), tui.curses.KEY_DOWN, state,
            self.Manager(), tui.WorkerState(),
        )
        self.assertTrue(state.history.follow)
        self.assertFalse(state.paused)
        self.assertEqual(state.unseen, 0)

    def test_filter_dialog_applies_one_category_and_escape_cancels(self) -> None:
        manager = self.Manager()
        state = self.state()
        apply_screen = self.Screen((ord("n"), ord(" "), 10))
        self.assertTrue(tui.modal_filter(apply_screen, state, manager))
        self.assertEqual(state.event_filter.enabled, {tui.FILTER_GROUPS[0][0]})

        original = set(state.event_filter.enabled)
        cancel_screen = self.Screen((ord("a"), 27))
        self.assertFalse(tui.modal_filter(cancel_screen, state, manager))
        self.assertEqual(state.event_filter.enabled, original)

    def test_paused_narrow_filter_and_append_preserve_viewport(self) -> None:
        manager = self.Manager()
        state = self.state()
        state.paused = True
        state.history.follow = False
        self.assertEqual(state.history.current().sequence, 2)

        jobs_index = list(tui.FILTER_KEYS).index("jobs")
        keys = [ord("n"), *([tui.curses.KEY_DOWN] * jobs_index),
                ord(" "), 10]
        self.assertTrue(tui.modal_filter(self.Screen(keys), state, manager))
        self.assertEqual(state.event_filter.enabled, {"jobs"})
        self.assertEqual(state.history.current(state.event_filter).sequence, 1)
        self.assertFalse(state.history.follow)
        self.assertTrue(state.paused)

        state.history.append(entry(3, event(3, "job.queued", job_id=3)))
        self.assertEqual(state.history.cursor_sequence, 1)
        self.assertEqual(state.history.current(state.event_filter).sequence, 1)
        self.assertFalse(state.history.follow)
        self.assertTrue(state.paused)

    def test_runtime_options_change_interval_and_event_rate(self) -> None:
        manager = self.Manager()
        state = self.state()
        screen = self.Screen((tui.curses.KEY_RIGHT, 10))
        self.assertTrue(tui.modal_options(screen, state, manager))
        self.assertEqual(state.interval, 6.0)

        screen = self.Screen((tui.curses.KEY_DOWN, tui.curses.KEY_RIGHT, 10))
        self.assertTrue(tui.modal_options(screen, state, manager))
        self.assertEqual(state.event_rate, 2.25)

    def test_export_key_writes_exactly_selected_rows(self) -> None:
        manager = self.Manager()
        workers = tui.WorkerState()
        state = self.state()
        state.focus = "events"
        state.history.toggle()
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "picked.json"
            with mock.patch.object(tui, "text_dialog", return_value=str(destination)):
                tui.handle_key(self.Screen(), ord("e"), state, manager, workers)
            exported = json.loads(destination.read_text(encoding="utf-8"))
        self.assertEqual(len(exported), 1)
        self.assertEqual(exported[0]["code"], "template.refreshed")

class CommandLineTests(unittest.TestCase):
    def run_cli(self, *arguments: str, timeout: float = 5.0) -> subprocess.CompletedProcess[str]:
        environment = {**os.environ, "LC_ALL": "C", "TERM": "dumb"}
        environment.pop("NO_COLOR", None)
        return subprocess.run(
            [sys.executable, str(TUI_PATH), *arguments],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
            env=environment,
        )

    def run_pty(self, *arguments: str,
                timeout: float = 5.0) -> tuple[int, bytes]:
        master, slave = pty.openpty()
        process: Optional[subprocess.Popen[bytes]] = None
        try:
            environment = {
                **os.environ, "TERM": "xterm-256color", "LC_ALL": "C",
            }
            environment.pop("NO_COLOR", None)
            process = subprocess.Popen(
                [sys.executable, str(TUI_PATH), *arguments],
                stdin=slave,
                stdout=slave,
                stderr=slave,
                close_fds=True,
                env=environment,
            )
            os.close(slave)
            slave = -1
            output = bytearray()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                readable, _, _ = select.select([master], [], [], 0.05)
                if not readable:
                    if process.poll() is not None:
                        break
                    continue
                try:
                    output.extend(os.read(master, 4096))
                except OSError:
                    break
                if process.poll() is not None:
                    readable, _, _ = select.select([master], [], [], 0)
                    if not readable:
                        break
            try:
                process.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=2.0)
                self.fail(f"PTY command timed out: {arguments!r}")
            return process.returncode, bytes(output)
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=2.0)
            if slave >= 0:
                os.close(slave)
            os.close(master)

    def test_help_documents_split_view_and_runtime_inputs(self) -> None:
        result = self.run_cli("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        for option in (
            "--view", "--reverse", "--layout", "--ui", "--theme",
            "--event-log", "--event-rate", "--stream-filter", "--from-start",
            "--api-url", "--api-token", "--interval", "--disk-path",
        ):
            self.assertIn(option, result.stdout)

    def test_invalid_arguments_exit_two_without_traceback(self) -> None:
        invalid = (
            ("--view", "middle"),
            ("--layout", "diagonal"),
            ("--ui", "glass"),
            ("--event-rate", "0"),
            ("--interval", "0"),
            ("--history-size", "0"),
            ("--theme", "not-a-theme"),
        )
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                result = self.run_cli(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("Traceback", result.stderr)

    def test_explicit_realistic_share_difficulty_floors_are_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "empty.jsonl"
            log.write_text("", encoding="utf-8")
            for difficulty in (4_198_404, 100_000_000):
                with self.subTest(difficulty=difficulty):
                    result = self.run_cli(
                        "--view", "events", "--ui", "tty", "--once",
                        "--event-log", str(log), "--from-start",
                        "--min-share-difficulty", str(difficulty),
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn("Traceback", result.stderr)

    def test_plain_event_view_smoke_reads_finite_jsonl(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "events.jsonl"
            log.write_text(
                json.dumps(event(1, "runtime.ready")) + "\n" +
                json.dumps(event(2, "share.completed", status="accepted",
                                 difficulty=900_000_000)) + "\n",
                encoding="utf-8",
            )
            result = self.run_cli(
                "--view", "events", "--ui", "tty", "--theme", "black",
                "--event-log", str(log), "--from-start", "--event-rate", "20",
                "--once", timeout=10.0,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("Traceback", result.stderr)
            self.assertTrue(result.stdout.strip())
            self.assertNotIn("\x1b", result.stdout)

    def test_tty_theme_colors_attached_output_but_not_redirected_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "events.jsonl"
            log.write_text(
                json.dumps(event(1, "runtime.ready")) + "\n",
                encoding="utf-8",
            )
            arguments = (
                "--view", "events", "--ui", "tty",
                "--theme", "windows-classic-tty",
                "--event-log", str(log), "--from-start",
                "--event-rate", "20", "--once",
            )
            redirected = self.run_cli(*arguments, timeout=10.0)
            self.assertEqual(redirected.returncode, 0, redirected.stderr)
            self.assertIn("RUNTIME.READY", redirected.stdout)
            self.assertNotIn("\x1b", redirected.stdout)

            returncode, attached = self.run_pty(*arguments, timeout=10.0)
            self.assertEqual(returncode, 0, attached.decode(errors="replace"))
            self.assertIn(b"RUNTIME.READY", attached)
            self.assertIn(b"\x1b[", attached)

    def test_pty_help_smoke(self) -> None:
        returncode, output = self.run_pty("--help")
        self.assertEqual(returncode, 0)
        self.assertIn(b"--view", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
