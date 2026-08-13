from __future__ import annotations

import argparse
import logging
import signal
import sys
import threading
from pathlib import Path
from typing import TextIO

from .ingest import ingest_path, ingest_stream
from .server import MetricsBridgeServer
from .store import MetricStore


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Serve calculation-engine NDJSON metrics to browsers over HTTP/SSE."
    )
    parser.add_argument(
        "--input",
        default="-",
        help="NDJSON input path, or - for calculation-engine stdout piped to stdin",
    )
    parser.add_argument(
        "--follow",
        action="store_true",
        help="keep following a growing input file after reaching EOF",
    )
    parser.add_argument("--archive", type=Path, help="append valid stdin metrics here")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--history-size", type=int, default=10_000)
    parser.add_argument("--client-queue-size", type=int, default=1_000)
    parser.add_argument("--heartbeat-seconds", type=float, default=15.0)
    parser.add_argument(
        "--cors-origin",
        action="append",
        dest="cors_origins",
        help="allowed browser Origin; repeat as needed (default: *)",
    )
    parser.add_argument(
        "--ui-dir",
        type=Path,
        help="serve this static browser-UI directory at / (default: API only)",
    )
    parser.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        default="INFO",
    )
    return parser.parse_args(argv)


def _start_ingestion(
    args: argparse.Namespace,
    store: MetricStore,
    stop_event: threading.Event,
) -> tuple[threading.Thread, TextIO | None]:
    archive: TextIO | None = None
    if args.archive is not None and args.input != "-":
        raise ValueError("--archive is only supported with --input -")
    if args.archive is not None:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        archive = args.archive.open("a", encoding="utf-8")

    if args.input == "-":
        thread = threading.Thread(
            target=ingest_stream,
            kwargs={
                "stream": sys.stdin,
                "store": store,
                "stop_event": stop_event,
                "follow": False,
                "archive": archive,
                "source_name": "<stdin>",
            },
            name="metrics-ingestion",
            daemon=True,
        )
    else:
        thread = threading.Thread(
            target=ingest_path,
            kwargs={
                "path": Path(args.input),
                "store": store,
                "stop_event": stop_event,
                "follow": args.follow,
            },
            name="metrics-ingestion",
            daemon=True,
        )
    thread.start()
    return thread, archive


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    stop_event = threading.Event()
    store = MetricStore(
        history_size=args.history_size,
        client_queue_size=args.client_queue_size,
    )
    server = MetricsBridgeServer(
        host=args.host,
        port=args.port,
        store=store,
        stop_event=stop_event,
        cors_origins=tuple(args.cors_origins or ("*",)),
        heartbeat_seconds=args.heartbeat_seconds,
        ui_dir=args.ui_dir,
    )
    ingestion_thread: threading.Thread | None = None
    archive: TextIO | None = None

    def request_shutdown(signum: int, frame: object) -> None:
        del signum, frame
        if stop_event.is_set():
            return
        stop_event.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGINT, request_shutdown)
    signal.signal(signal.SIGTERM, request_shutdown)

    try:
        ingestion_thread, archive = _start_ingestion(args, store, stop_event)
        host, port = server.address
        print(f"metrics bridge listening on http://{host}:{port}", file=sys.stderr)
        if args.ui_dir is not None:
            print(f"metrics UI served at http://{host}:{port}/", file=sys.stderr)
        server.serve_forever()
        return 0
    finally:
        stop_event.set()
        if ingestion_thread is not None:
            ingestion_thread.join(timeout=1.0)
        if archive is not None:
            archive.close()
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
