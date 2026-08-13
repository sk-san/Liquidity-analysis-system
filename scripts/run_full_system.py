#!/usr/bin/env python3
"""Run ABIDES, pacing, calculation, and browser metrics services as one job."""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import IO, Any


ROOT = Path(__file__).resolve().parents[1]
LOCAL_PACKAGE_PATHS = (
    ROOT,
    ROOT / "packages" / "market-data-protocol" / "src",
    ROOT / "components" / "pacing-server" / "src",
    ROOT / "components" / "metrics-bridge" / "src",
    ROOT / "components" / "abides" / "extensions" / "market_data_emitter" / "src",
)
for package_path in reversed(LOCAL_PACKAGE_PATHS):
    sys.path.insert(0, str(package_path))

import zmq

from market_data_protocol import (
    control_message,
    decode_message,
    encode_message,
)


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--scenario", default="rmsc04")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--end-time", default="10:00:00")
    parser.add_argument("--ingress", default="tcp://127.0.0.1:5557")
    parser.add_argument("--egress", default="tcp://127.0.0.1:5558")
    parser.add_argument("--control", default="tcp://127.0.0.1:5559")
    parser.add_argument("--pacing-speed", type=float, default=1.0)
    parser.add_argument("--metrics-host", default="127.0.0.1")
    parser.add_argument("--metrics-port", type=int, default=8765)
    parser.add_argument("--metrics-history-size", type=int, default=10_000)
    parser.add_argument(
        "--ui-dir",
        type=Path,
        default=ROOT / "components" / "metrics-ui",
        help="static browser UI served by the metrics bridge at /",
    )
    parser.add_argument(
        "--no-ui",
        action="store_true",
        help="serve the metrics API without the browser UI",
    )
    parser.add_argument(
        "--hold",
        action="store_true",
        help="keep the metrics bridge and UI serving after the run drains",
    )
    parser.add_argument("--state-dir", type=Path, default=Path("state/full-system"))
    parser.add_argument("--startup-timeout", type=float, default=10.0)
    parser.add_argument("--drain-timeout", type=float, default=15.0)
    return parser.parse_args()


def _pythonpath(environment: dict[str, str]) -> str:
    paths = [
        ROOT,
        ROOT / "components" / "abides" / "abides-core",
        ROOT / "components" / "abides" / "abides-markets",
        *LOCAL_PACKAGE_PATHS[1:],
    ]
    current = environment.get("PYTHONPATH")
    values = [str(path) for path in paths]
    if current:
        values.append(current)
    return os.pathsep.join(dict.fromkeys(values))


def _control_request(
    endpoint: str, command: str, *, timeout_ms: int = 500
) -> dict[str, Any]:
    context = zmq.Context.instance()
    socket = context.socket(zmq.REQ)
    socket.setsockopt(zmq.LINGER, 0)
    socket.setsockopt(zmq.SNDTIMEO, timeout_ms)
    socket.setsockopt(zmq.RCVTIMEO, timeout_ms)
    socket.connect(endpoint)
    try:
        socket.send(encode_message(control_message("CONTROL", command=command)))
        response = decode_message(socket.recv())
    finally:
        socket.close(linger=0)
    if response["kind"] != "CONTROL_RESULT":
        raise RuntimeError(f"unexpected pacing control response: {response['kind']}")
    return dict(response["body"])


def _wait_for_pacing(
    process: subprocess.Popen[bytes], endpoint: str, timeout: float
) -> None:
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"pacing server exited during startup with code {process.returncode}"
            )
        try:
            response = _control_request(endpoint, "stats")
            if response.get("ok"):
                return
        except (RuntimeError, zmq.ZMQError) as error:
            last_error = error
        time.sleep(0.1)
    raise RuntimeError("pacing server did not become ready") from last_error


def _wait_for_metrics_bridge(
    process: subprocess.Popen[bytes], host: str, port: int, timeout: float
) -> None:
    connect_host = "127.0.0.1" if host in ("0.0.0.0", "::") else host
    endpoint = f"http://{connect_host}:{port}/healthz"
    deadline = time.monotonic() + timeout
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                "metrics bridge exited during startup with code "
                f"{process.returncode}"
            )
        try:
            with urllib.request.urlopen(endpoint, timeout=0.5) as response:
                if response.status == 200:
                    return
        except (OSError, urllib.error.URLError) as error:
            last_error = error
        time.sleep(0.1)
    raise RuntimeError("metrics bridge did not become ready") from last_error


def _wait_for_drain(
    pacing: subprocess.Popen[bytes],
    engine: subprocess.Popen[bytes],
    metrics_bridge: subprocess.Popen[bytes],
    endpoint: str,
    timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    latest: dict[str, Any] = {}
    while time.monotonic() < deadline:
        if pacing.poll() is not None:
            raise RuntimeError(
                f"pacing server exited with code {pacing.returncode}"
            )
        if engine.poll() is not None:
            raise RuntimeError(
                f"calculation engine exited with code {engine.returncode}"
            )
        if metrics_bridge.poll() is not None:
            raise RuntimeError(
                f"metrics bridge exited with code {metrics_bridge.returncode}"
            )
        response = _control_request(endpoint, "stats")
        latest = dict(response.get("stats", {}))
        total = int(latest.get("journal_total", 0))
        delivered = int(latest.get("journal_delivered", 0))
        queued = int(latest.get("delivery_queue_size", 0))
        if total > 0 and delivered == total and queued == 0:
            time.sleep(0.5)
            return latest
        time.sleep(0.1)
    raise RuntimeError(f"timed out draining pacing server; last stats={latest}")


def _stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5.0)


def _open_log(path: Path) -> IO[bytes]:
    return path.open("wb")


def _validate_engine_log(path: Path) -> None:
    failure_markers = (
        " was rejected:",
        " was gap_detected:",
        " was desynchronized:",
        "discarding invalid market-data frame",
        "transport sequence gap:",
    )
    contents = path.read_text(encoding="utf-8", errors="replace")
    failures = [
        line
        for line in contents.splitlines()
        if any(marker in line for marker in failure_markers)
    ]
    if failures:
        preview = "; ".join(failures[:3])
        raise RuntimeError(
            f"calculation engine rejected market data ({preview}); see {path}"
        )


def main() -> int:
    args = _parse_args()
    engine_path = args.engine.resolve()
    if not engine_path.is_file():
        raise FileNotFoundError(f"calculation engine not found: {engine_path}")
    if args.pacing_speed <= 0:
        raise ValueError("--pacing-speed must be positive")
    if not 1 <= args.metrics_port <= 65_535:
        raise ValueError("--metrics-port must be between 1 and 65535")
    if args.metrics_history_size <= 0:
        raise ValueError("--metrics-history-size must be positive")
    ui_dir: Path | None = None if args.no_ui else args.ui_dir.resolve()
    if ui_dir is not None and not (ui_dir / "index.html").is_file():
        raise FileNotFoundError(f"metrics UI not found: {ui_dir}")

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_id = f"{args.scenario}-{timestamp}-{os.getpid()}"
    run_dir = (ROOT / args.state_dir).resolve()
    run_dir = run_dir / run_id
    run_dir.mkdir(parents=True, exist_ok=False)

    environment = os.environ.copy()
    environment["PYTHONPATH"] = _pythonpath(environment)
    environment.update(
        {
            "ABIDES_MARKET_DATA_ENABLED": "1",
            "ABIDES_MARKET_DATA_RUN_ID": run_id,
            "ABIDES_MARKET_DATA_ENDPOINT": args.ingress,
            "ABIDES_MARKET_DATA_JOURNAL": str(run_dir / "emitter.sqlite3"),
        }
    )

    pacing_log = _open_log(run_dir / "pacing.log")
    engine_log = _open_log(run_dir / "calculation-engine.log")
    metrics_bridge_log = _open_log(run_dir / "metrics-bridge.log")
    simulation_log = _open_log(run_dir / "simulation.log")
    pacing: subprocess.Popen[bytes] | None = None
    engine: subprocess.Popen[bytes] | None = None
    metrics_bridge: subprocess.Popen[bytes] | None = None
    metrics_bridge_input: IO[bytes] | None = None

    print(f"Full-system run: {run_id}", flush=True)
    print(f"Artifacts: {run_dir}", flush=True)

    try:
        metrics_bridge = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "metrics_bridge.cli",
                "--input",
                "-",
                "--archive",
                str(run_dir / "metrics.ndjson"),
                "--host",
                args.metrics_host,
                "--port",
                str(args.metrics_port),
                "--history-size",
                str(args.metrics_history_size),
                *(("--ui-dir", str(ui_dir)) if ui_dir is not None else ()),
            ],
            cwd=ROOT,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=metrics_bridge_log,
            stderr=subprocess.STDOUT,
        )
        metrics_bridge_input = metrics_bridge.stdin
        if metrics_bridge_input is None:
            raise RuntimeError("metrics bridge stdin pipe was not created")
        _wait_for_metrics_bridge(
            metrics_bridge,
            args.metrics_host,
            args.metrics_port,
            args.startup_timeout,
        )
        print(
            "Metrics bridge: "
            f"http://{args.metrics_host}:{args.metrics_port}/api/v1/metrics/stream",
            flush=True,
        )
        if ui_dir is not None:
            print(
                f"Metrics UI: http://{args.metrics_host}:{args.metrics_port}/",
                flush=True,
            )

        pacing = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "pacing_server.cli",
                "--ingress",
                args.ingress,
                "--egress",
                args.egress,
                "--control",
                args.control,
                "--journal",
                str(run_dir / "pacing.sqlite3"),
                "--speed",
                str(args.pacing_speed),
            ],
            cwd=ROOT,
            env=environment,
            stdout=pacing_log,
            stderr=subprocess.STDOUT,
        )
        _wait_for_pacing(pacing, args.control, args.startup_timeout)

        engine = subprocess.Popen(
            [str(engine_path), "--endpoint", args.egress],
            cwd=ROOT,
            env=environment,
            stdout=metrics_bridge_input,
            stderr=engine_log,
        )
        metrics_bridge_input.close()
        metrics_bridge_input = None
        time.sleep(0.25)
        if engine.poll() is not None:
            raise RuntimeError(
                f"calculation engine exited during startup with code {engine.returncode}"
            )

        print(
            f"Running {args.scenario.upper()} through {args.end_time} "
            f"(seed={args.seed})",
            flush=True,
        )
        simulation = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "run_abides_sim.py"),
                "--scenario",
                args.scenario,
                "--seed",
                str(args.seed),
                "--end-time",
                args.end_time,
            ],
            cwd=ROOT,
            env=environment,
            stdout=simulation_log,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if simulation.returncode != 0:
            raise RuntimeError(
                "ABIDES simulation failed; see "
                f"{run_dir / 'simulation.log'}"
            )

        stats = _wait_for_drain(
            pacing,
            engine,
            metrics_bridge,
            args.control,
            args.drain_timeout,
        )
        _validate_engine_log(run_dir / "calculation-engine.log")
        metric_count = sum(
            1
            for line in (run_dir / "metrics.ndjson").read_bytes().splitlines()
            if line.strip()
        )
        print(
            "Pipeline drained: "
            f"{stats.get('journal_delivered', 0)}/"
            f"{stats.get('journal_total', 0)} messages delivered",
            flush=True,
        )
        print(
            f"Metrics: {metric_count} records in "
            f"{run_dir / 'metrics.ndjson'}",
            flush=True,
        )
        if args.hold:
            _stop_process(engine)
            engine = None
            try:
                _control_request(args.control, "stop")
            except (RuntimeError, zmq.ZMQError):
                pass
            _stop_process(pacing)
            pacing = None
            print(
                "Holding: metrics UI stays at "
                f"http://{args.metrics_host}:{args.metrics_port}/ "
                "(Ctrl-C to stop)",
                flush=True,
            )
            try:
                while metrics_bridge.poll() is None:
                    time.sleep(0.5)
            except KeyboardInterrupt:
                pass
        return 0
    finally:
        if metrics_bridge_input is not None:
            metrics_bridge_input.close()
        _stop_process(engine)
        _stop_process(metrics_bridge)
        if pacing is not None and pacing.poll() is None:
            try:
                _control_request(args.control, "stop")
            except (RuntimeError, zmq.ZMQError):
                pass
        _stop_process(pacing)
        simulation_log.close()
        metrics_bridge_log.close()
        engine_log.close()
        pacing_log.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as error:
        print(f"run_full_system: {error}", file=sys.stderr)
        raise SystemExit(1)
