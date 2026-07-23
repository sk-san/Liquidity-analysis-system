from __future__ import annotations

import argparse
import logging
import signal
import threading
from pathlib import Path

from .config import PacingServerConfig
from .server import PacingServer


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ABIDES market-data pacing server")
    parser.add_argument("--ingress", default="tcp://127.0.0.1:5557")
    parser.add_argument("--egress", default="tcp://127.0.0.1:5558")
    parser.add_argument("--control", default="tcp://127.0.0.1:5559")
    parser.add_argument("--journal", type=Path, default=Path("state/pacing-server.sqlite3"))
    parser.add_argument("--speed", type=float, default=1.0)
    parser.add_argument("--start-paused", action="store_true")
    parser.add_argument("--log-level", default="INFO")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level.upper()),
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    server = PacingServer(
        PacingServerConfig(
            ingress_endpoint=args.ingress,
            egress_endpoint=args.egress,
            control_endpoint=args.control,
            journal_path=args.journal,
            playback_speed=args.speed,
            start_paused=args.start_paused,
        )
    )
    stopped = threading.Event()

    def stop_handler(signum: int, frame: object) -> None:
        del signum, frame
        stopped.set()

    signal.signal(signal.SIGINT, stop_handler)
    signal.signal(signal.SIGTERM, stop_handler)
    server.start()
    try:
        while not stopped.wait(0.25):
            if server.last_error is not None:
                raise RuntimeError("pacing server thread failed") from server.last_error
    finally:
        server.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
