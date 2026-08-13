from __future__ import annotations

import logging
import threading
from pathlib import Path
from typing import TextIO

from .model import MetricValidationError, parse_metric_line
from .store import MetricStore


logger = logging.getLogger(__name__)

ZMQ_SCHEMES = ("tcp://", "ipc://", "inproc://")


def is_zmq_endpoint(value: str) -> bool:
    return value.startswith(ZMQ_SCHEMES)


def ingest_stream(
    stream: TextIO,
    store: MetricStore,
    *,
    stop_event: threading.Event,
    follow: bool = False,
    poll_interval: float = 0.05,
    archive: TextIO | None = None,
    source_name: str = "<stream>",
) -> None:
    line_number = 0
    partial_line = ""
    try:
        while not stop_event.is_set():
            line = stream.readline()
            if not line:
                if follow:
                    stop_event.wait(poll_interval)
                    continue
                break
            if partial_line:
                line = partial_line + line
                partial_line = ""
            if follow and not line.endswith(("\n", "\r")):
                # A regular file can reach its temporary EOF while the producer
                # is still writing one JSON record. Retain that fragment instead
                # of reporting a false parse error and losing the metric.
                partial_line = line
                stop_event.wait(poll_interval)
                continue
            line_number += 1
            stripped = line.strip()
            if not stripped:
                continue
            try:
                metric = parse_metric_line(stripped)
            except MetricValidationError as error:
                store.record_invalid()
                logger.warning(
                    "discarding invalid metric at %s:%d: %s",
                    source_name,
                    line_number,
                    error,
                )
                continue
            if archive is not None:
                archive.write(stripped + "\n")
                archive.flush()
            store.publish(metric)
    finally:
        store.mark_source_eof()


def ingest_zmq(
    endpoint: str,
    store: MetricStore,
    *,
    stop_event: threading.Event,
    poll_interval: float = 0.25,
    archive: TextIO | None = None,
) -> None:
    """Bind a ZeroMQ PULL socket and ingest one metric record per message.

    pyzmq is an optional dependency: stdin and file input work without it.
    """
    try:
        import zmq
    except ImportError as error:  # pragma: no cover - environment-specific
        raise RuntimeError(
            "ZeroMQ input requires pyzmq "
            "(pip install 'liquidity-metrics-bridge[zmq]')"
        ) from error

    socket = zmq.Context.instance().socket(zmq.PULL)
    socket.setsockopt(zmq.RCVHWM, 100_000)
    socket.setsockopt(zmq.LINGER, 0)
    socket.bind(endpoint)
    record_number = 0
    try:
        while not stop_event.is_set():
            if socket.poll(timeout=int(poll_interval * 1000)) == 0:
                continue
            payload = socket.recv()
            record_number += 1
            try:
                stripped = payload.decode("utf-8").strip()
            except UnicodeDecodeError as error:
                store.record_invalid()
                logger.warning(
                    "discarding undecodable metric at %s #%d: %s",
                    endpoint,
                    record_number,
                    error,
                )
                continue
            if not stripped:
                continue
            try:
                metric = parse_metric_line(stripped)
            except MetricValidationError as error:
                store.record_invalid()
                logger.warning(
                    "discarding invalid metric at %s #%d: %s",
                    endpoint,
                    record_number,
                    error,
                )
                continue
            if archive is not None:
                archive.write(stripped + "\n")
                archive.flush()
            store.publish(metric)
    finally:
        socket.close(linger=0)
        store.mark_source_eof()


def ingest_path(
    path: Path,
    store: MetricStore,
    *,
    stop_event: threading.Event,
    follow: bool,
    poll_interval: float = 0.05,
) -> None:
    while not path.exists():
        if not follow or stop_event.wait(poll_interval):
            store.mark_source_eof()
            return
    with path.open("r", encoding="utf-8") as stream:
        ingest_stream(
            stream,
            store,
            stop_event=stop_event,
            follow=follow,
            poll_interval=poll_interval,
            source_name=str(path),
        )
