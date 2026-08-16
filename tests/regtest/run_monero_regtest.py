#!/usr/bin/env python3
"""Run one verified share/candidate against a real Monero fakechain daemon.

The harness uses only Python's standard library.  It starts the pinned
``monerod`` and the locally built server in one process namespace, obtains a
private Stratum job, computes the exact RandomX result with ``mspv_verify``,
submits it, and checks both daemon advancement and durable SQLite state.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen


WALLET_ADDRESS = (
    "44AFFq5kSiGBoZ4NMDwYtN18obc8AemS33DBLWs3H7otXft3XjrpDtQGv7SqSsa"
    "BYBb98uNbr2VBBEt7f2wfn3RVGQBEP3A"
)
PASSWORD = "monero-regtest-password"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def distinct_ports(count: int) -> list[int]:
    ports: list[int] = []
    while len(ports) < count:
        port = free_port()
        if port not in ports:
            ports.append(port)
    return ports


def rpc(port: int, method: str, params: Any) -> dict[str, Any]:
    payload = json.dumps(
        {"jsonrpc": "2.0", "id": "regtest", "method": method, "params": params}
    ).encode()
    request = Request(
        f"http://127.0.0.1:{port}/json_rpc",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urlopen(request, timeout=5) as response:
        document = json.loads(response.read())
    require(document.get("error") in (None, {}), f"{method} returned an RPC error")
    require(isinstance(document.get("result"), dict), f"{method} omitted result")
    return document["result"]


def wait_rpc(port: int, timeout: float = 20.0) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            return rpc(port, "get_info", {})
        except (OSError, URLError, ValueError, RuntimeError) as error:
            last_error = error
            time.sleep(0.1)
    raise RuntimeError(f"monerod RPC did not become ready: {last_error}")


def http_json(port: int, path: str) -> tuple[int, dict[str, Any]]:
    request = Request(f"http://127.0.0.1:{port}{path}", method="GET")
    with urlopen(request, timeout=5) as response:
        return int(response.status), json.loads(response.read())


def wait_server(port: int, process: subprocess.Popen[bytes], timeout: float = 30.0) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"server exited during startup with status {process.returncode}")
        try:
            status, document = http_json(port, "/v1/health/ready")
            if status == 200 and document.get("data", {}).get("ready") is True:
                return
        except (OSError, URLError, ValueError, RuntimeError) as error:
            last_error = error
        time.sleep(0.1)
    raise RuntimeError(f"server did not become ready: {last_error}")


def read_json_line(stream: Any) -> dict[str, Any]:
    encoded = stream.readline()
    require(bool(encoded), "Stratum connection closed before a response")
    return json.loads(encoded)


def read_response(stream: Any, expected_id: object) -> dict[str, Any]:
    """Read one response while allowing asynchronous Stratum job notices."""
    for _ in range(64):
        document = read_json_line(stream)
        if document.get("id") == expected_id:
            return document
        require(
            document.get("method") == "job" and "id" not in document,
            f"unexpected Stratum message while awaiting {expected_id!r}: {document}",
        )
    raise RuntimeError(f"too many job notices while awaiting {expected_id!r}")


def nonce_offset(blob: bytes) -> int:
    position = 0
    for _ in range(3):
        for byte_index in range(10):
            require(position < len(blob), "truncated Stratum job header")
            byte = blob[position]
            position += 1
            if byte & 0x80 == 0:
                break
            require(byte_index != 9, "oversized Stratum job varint")
        else:
            raise RuntimeError("unterminated Stratum job varint")
    position += 32
    require(position + 4 <= len(blob), "Stratum job omitted the nonce")
    return position


def compute_hash(mspv_verify: Path, seed_hex: str, blob_hex: str, nonce_hex: str) -> str:
    blob = bytearray.fromhex(blob_hex)
    nonce = bytes.fromhex(nonce_hex)
    require(len(nonce) == 4, "test nonce is not four bytes")
    offset = nonce_offset(blob)
    blob[offset : offset + 4] = nonce
    completed = subprocess.run(
        [str(mspv_verify), "light", seed_hex, blob.hex()],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=120,
    )
    require(
        completed.returncode == 0,
        "mspv_verify failed:\n" + completed.stdout + completed.stderr,
    )
    for line in completed.stdout.splitlines():
        if line.startswith("hash: "):
            result = line.removeprefix("hash: ").strip()
            require(len(result) == 64, "mspv_verify returned a malformed hash")
            return result
    raise RuntimeError("mspv_verify did not print a hash")


def scalar(database: sqlite3.Connection, query: str) -> int:
    row = database.execute(query).fetchone()
    require(row is not None, f"query returned no row: {query}")
    return int(row[0])


def terminate(process: subprocess.Popen[bytes] | None, sig: signal.Signals) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(sig)
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)


def run(args: argparse.Namespace) -> None:
    server_binary = args.server.resolve()
    monerod_binary = args.monerod.resolve()
    mspv_binary = args.mspv_verify.resolve()
    for binary in (server_binary, monerod_binary, mspv_binary):
        require(binary.is_file() and os.access(binary, os.X_OK), f"not executable: {binary}")

    rpc_port, p2p_port, stratum_port, api_port = distinct_ports(4)
    with tempfile.TemporaryDirectory(prefix="monero-solo-regtest-") as temporary:
        root = Path(temporary)
        data_dir = root / "monerod"
        database_path = root / "server.sqlite3"
        config_path = root / "config.json"
        monerod_log = (root / "monerod.log").open("wb")
        server_log = (root / "server.log").open("wb")
        config = {
            "schema_version": 2,
            "network": "regtest",
            "wallet_address": WALLET_ADDRESS,
            "blocknotify": None,
            "stratum": {
                "listen": [f"127.0.0.1:{stratum_port}"],
                "access_password": PASSWORD,
                "job_ttl_ms": 120000,
                "max_pending_verifications_per_connection": 8,
            },
            "daemon": {
                "rpc_url": f"http://127.0.0.1:{rpc_port}",
                "rpc_username": None,
                "rpc_password": None,
                "zmq_address": None,
                "poll_interval_ms": 1000,
                "request_timeout_ms": 10000,
                "submit_attempts": 4,
                "submit_retry_ms": 250,
            },
            "difficulty": {"mode": "fixed", "value": 1},
            "verifier": {
                "enabled": True,
                "memory_mode": "light",
                "workers": 1,
                "seed_init_threads": 1,
                "max_seeds": 2,
                "pending_capacity": 16,
                "max_outstanding": 32,
                "large_pages": "disabled",
                "jit": "secure",
                "aes": "auto",
                "log_level": "warning",
            },
            "entropy": {},
            "database": {
                "path": str(database_path),
                "min_persisted_share_difficulty": 80000000000,
                "accounting_flush_interval_ms": 1000,
            },
            "events": {
                "enabled": False,
                "unix_socket": str(root / "events.sock"),
            },
            "api": {"enabled": True, "listen": f"127.0.0.1:{api_port}"},
            "defense": {
                "enabled": False,
                "candidate_rate_per_minute": 0,
                "candidate_burst": 0,
                "candidate_inflight_per_ip": 0,
                "candidate_global_inflight": 0,
            },
            "logging": {"level": "info", "file": None},
        }
        config_path.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")

        monerod: subprocess.Popen[bytes] | None = None
        server: subprocess.Popen[bytes] | None = None
        try:
            monerod = subprocess.Popen(
                [
                    str(monerod_binary),
                    "--regtest",
                    "--offline",
                    "--fixed-difficulty",
                    "1",
                    "--keep-fakechain",
                    "--data-dir",
                    str(data_dir),
                    "--rpc-bind-ip",
                    "127.0.0.1",
                    "--rpc-bind-port",
                    str(rpc_port),
                    "--p2p-bind-ip",
                    "127.0.0.1",
                    "--p2p-bind-port",
                    str(p2p_port),
                    "--no-zmq",
                    "--no-igd",
                    "--non-interactive",
                    "--disable-dns-checkpoints",
                    "--check-updates",
                    "disabled",
                    "--log-level",
                    "1",
                ],
                stdout=monerod_log,
                stderr=subprocess.STDOUT,
            )
            info = wait_rpc(rpc_port)
            require(info.get("nettype") == "fakechain", "monerod did not report fakechain")
            initial_height = int(info["height"])

            template = rpc(
                rpc_port,
                "getblocktemplate",
                {"wallet_address": WALLET_ADDRESS, "reserve_size": 16},
            )
            require(template.get("status") == "OK", "real template status was not OK")
            require(template.get("next_seed_hash") == "", "unexpected genesis next seed hash")
            require(isinstance(template.get("seed_height"), int), "template omitted seed_height")

            server = subprocess.Popen(
                [str(server_binary), "--config", str(config_path)],
                stdout=server_log,
                stderr=subprocess.STDOUT,
            )
            wait_server(api_port, server)

            with socket.create_connection(("127.0.0.1", stratum_port), timeout=5) as connection:
                connection.settimeout(30)
                stream = connection.makefile("rwb", buffering=0)
                login = {
                    "id": 1,
                    "jsonrpc": "2.0",
                    "method": "login",
                    "params": {
                        "login": "real-regtest",
                        "pass": PASSWORD,
                        "agent": "mss-regtest-harness/1",
                        "rigid": "real-monero",
                        "algo": ["rx/0"],
                    },
                }
                stream.write((json.dumps(login) + "\n").encode())
                login_response = read_json_line(stream)
                require(login_response.get("error") is None, f"login failed: {login_response}")
                result = login_response["result"]
                job = result["job"]
                require(job.get("algo") == "rx/0", "server returned a non-RandomX job")
                nonce_hex = "01020304"
                computed_hash = compute_hash(
                    mspv_binary, str(job["seed_hash"]), str(job["blob"]), nonce_hex
                )
                submit = {
                    "id": "verified-candidate-1",
                    "jsonrpc": "2.0",
                    "method": "submit",
                    "params": {
                        "id": result["id"],
                        "job_id": job["job_id"],
                        "nonce": nonce_hex,
                        "result": computed_hash,
                        "algo": "rx/0",
                    },
                }
                stream.write((json.dumps(submit) + "\n").encode())
                submit_response = read_response(stream, submit["id"])
                require(
                    submit_response.get("error") is None
                    and submit_response.get("result", {}).get("status") == "OK",
                    f"verified share was not accepted: {submit_response}",
                )

            deadline = time.monotonic() + 20
            final_height = initial_height
            while time.monotonic() < deadline:
                final_height = int(rpc(rpc_port, "get_info", {})["height"])
                if final_height > initial_height:
                    break
                time.sleep(0.1)
            require(final_height == initial_height + 1, "candidate did not advance fakechain once")

            terminate(server, signal.SIGTERM)
            require(server.returncode == 0, f"server stopped with status {server.returncode}")
            server = None

            with sqlite3.connect(database_path) as database:
                require(database.execute("PRAGMA integrity_check").fetchone() == ("ok",),
                        "SQLite integrity_check failed")
                require(not database.execute("PRAGMA foreign_key_check").fetchall(),
                        "SQLite foreign_key_check failed")
                require(scalar(database, "SELECT count(*) FROM shares WHERE status='accepted' AND provenance='verified' AND retention_reason='candidate'") == 1,
                        "verified candidate share was not durable")
                require(scalar(database, "SELECT sum(share_count) FROM share_totals WHERE status='accepted' AND provenance='verified'") == 1,
                        "verified accepted share was not compactly accounted")
                require(scalar(database, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('public_templates','private_jobs','duplicate_keys')") == 0,
                        "removed transient tables were recreated")
                require(scalar(database, "SELECT count(*) FROM candidates WHERE state='accepted'") == 1,
                        "accepted candidate was not durable")
                require(scalar(database, "SELECT count(*) FROM candidate_attempts") == 1,
                        "candidate was dispatched more or less than once")
                require(scalar(database, "SELECT count(*) FROM server_sessions WHERE clean_shutdown=1") == 1,
                        "regtest session was not recorded as a clean shutdown")

            print(
                json.dumps(
                    {
                        "result": "pass",
                        "monerod": "v0.18.5.1-release",
                        "network": "fakechain",
                        "initial_height": initial_height,
                        "final_height": final_height,
                        "share": "accepted/verified",
                        "candidate_attempts": 1,
                    },
                    sort_keys=True,
                )
            )
        except Exception:
            monerod_log.flush()
            server_log.flush()
            print(f"regtest logs retained until process exit under {root}", file=sys.stderr)
            for label, path in (("server", root / "server.log"), ("monerod", root / "monerod.log")):
                if path.exists():
                    print(f"--- {label}.log ---", file=sys.stderr)
                    print(path.read_text(encoding="utf-8", errors="replace")[-12000:], file=sys.stderr)
            raise
        finally:
            terminate(server, signal.SIGTERM)
            terminate(monerod, signal.SIGINT)
            monerod_log.close()
            server_log.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True,
                        help="built monero-solo-stratum executable")
    parser.add_argument("--monerod", type=Path, required=True,
                        help="pinned Monero v0.18.5.1 monerod executable")
    parser.add_argument("--mspv-verify", type=Path, required=True,
                        help="mspv_verify from the exact vendored verifier")
    arguments = parser.parse_args()
    try:
        run(arguments)
        return 0
    except Exception as error:
        print(f"real Monero regtest failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
