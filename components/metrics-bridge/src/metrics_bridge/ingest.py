from __future__ import annotations

import logging
import threading
from pathlib import Path
from typing import TextIO

from .model import MetricValidationError, parse_metric_line
from .store import MetricStore


logger = logging.getLogger(__name__)


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
