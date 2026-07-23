from __future__ import annotations

import logging
import os
import threading
import time
import uuid
from collections import OrderedDict
from collections.abc import Mapping, Sequence
from typing import Any

import zmq

from market_data_protocol import (
    INCREMENTAL_EVENT_TYPES,
    LIFECYCLE_EVENT_TYPES,
    MarketDataEnvelope,
    control_message,
    decode_message,
    encode_message,
    market_data_message,
)

from .config import EmitterConfig
from .journal import EmitterJournal

logger = logging.getLogger(__name__)


class EmitterBackpressureError(RuntimeError):
    pass


class MarketDataEmitter:
    """Durable asynchronous ZeroMQ emitter intended to be owned by ABIDES.

    ABIDES calls ``emit_*`` from its deterministic simulation thread. The method
    validates and writes a canonical frame to a local SQLite outbox. A dedicated
    thread owns the ZeroMQ DEALER socket and handles HELLO/READY, retries and
    cumulative ACKs. Network I/O never executes in the simulation thread.
    """

    def __init__(
        self,
        *,
        run_id: str,
        config: EmitterConfig | None = None,
        context: zmq.Context | None = None,
    ) -> None:
        if not run_id:
            raise ValueError("run_id must not be empty")
        self.run_id = run_id
        self.config = config or EmitterConfig()
        self.config.validate()
        self._context = context or zmq.Context.instance()
        self._journal = EmitterJournal(self.config.journal_path)
        journal_run_id = self._journal.get_text("run_id")
        if journal_run_id is not None and journal_run_id != run_id:
            self._journal.close()
            raise ValueError(
                f"emitter journal belongs to run_id={journal_run_id!r}; "
                f"use a separate journal for run_id={run_id!r}"
            )
        self._journal.set_text("run_id", run_id)
        self._transport_sequence = self._journal.last_transport_sequence()
        self._source_sequences: dict[str, int] = {}
        self._sequence_lock = threading.RLock()
        self._condition = threading.Condition()
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_ack = self._journal.last_acknowledged_sequence()
        self._last_error: BaseException | None = None

    @property
    def last_transport_sequence(self) -> int:
        with self._sequence_lock:
            return self._transport_sequence

    @property
    def last_acknowledged_sequence(self) -> int:
        with self._condition:
            return self._last_ack

    @property
    def pending_count(self) -> int:
        return self._journal.pending_count()

    @property
    def last_error(self) -> BaseException | None:
        return self._last_error

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._publisher_loop,
            name=f"market-data-emitter-{self.run_id}",
            daemon=True,
        )
        self._thread.start()

    def close(self, *, flush: bool = True, timeout: float = 10.0) -> None:
        flush_error: BaseException | None = None
        try:
            if flush and self._thread and self._thread.is_alive():
                self.flush(timeout=timeout)
        except BaseException as exc:
            flush_error = exc
        finally:
            self._stop_event.set()
            with self._condition:
                self._condition.notify_all()
            if self._thread:
                self._thread.join(timeout=timeout)
            self._journal.close()
        if flush_error is not None:
            raise flush_error

    def __enter__(self) -> "MarketDataEmitter":
        self.start()
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.close(flush=exc is None)

    def flush(self, *, timeout: float = 10.0) -> bool:
        deadline = time.monotonic() + timeout
        target = self.last_transport_sequence
        with self._condition:
            while self._last_ack < target:
                if self._last_error is not None:
                    raise RuntimeError("publisher thread failed") from self._last_error
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._condition.notify_all()
                self._condition.wait(timeout=min(remaining, 0.25))
        return True

    def emit_incremental(
        self,
        *,
        channel_id: str,
        symbol: str,
        sim_time_ns: int,
        event_type: str,
        payload: Mapping[str, Any],
    ) -> MarketDataEnvelope:
        if event_type not in INCREMENTAL_EVENT_TYPES:
            raise ValueError(f"not an incremental event type: {event_type}")
        source_sequence = self._next_source_sequence(channel_id)
        return self._emit(
            channel_id=channel_id,
            symbol=symbol,
            source_sequence=source_sequence,
            sim_time_ns=sim_time_ns,
            event_type=event_type,
            payload=payload,
        )

    def emit_snapshot(
        self,
        *,
        channel_id: str,
        symbol: str,
        sim_time_ns: int,
        orders: Sequence[Mapping[str, Any]],
        last_trade_price: int | None = None,
    ) -> MarketDataEnvelope:
        # A snapshot represents the current state after the latest incremental;
        # it intentionally reuses the current per-channel source watermark.
        source_sequence = self._current_source_sequence(channel_id)
        return self._emit(
            channel_id=channel_id,
            symbol=symbol,
            source_sequence=source_sequence,
            sim_time_ns=sim_time_ns,
            event_type="SNAPSHOT",
            payload={
                "orders": [dict(order) for order in orders],
                "last_trade_price": last_trade_price,
            },
        )

    def emit_lifecycle(
        self,
        *,
        event_type: str,
        sim_time_ns: int,
        payload: Mapping[str, Any] | None = None,
        channel_id: str = "SYSTEM",
    ) -> MarketDataEnvelope:
        if event_type not in LIFECYCLE_EVENT_TYPES:
            raise ValueError(f"not a lifecycle event type: {event_type}")
        return self._emit(
            channel_id=channel_id,
            symbol="",
            source_sequence=self._current_source_sequence(channel_id),
            sim_time_ns=sim_time_ns,
            event_type=event_type,
            payload=payload or {},
        )

    def _emit(
        self,
        *,
        channel_id: str,
        symbol: str,
        source_sequence: int,
        sim_time_ns: int,
        event_type: str,
        payload: Mapping[str, Any],
    ) -> MarketDataEnvelope:
        self._wait_for_capacity()
        now_ns = time.time_ns()
        with self._sequence_lock:
            self._transport_sequence += 1
            transport_sequence = self._transport_sequence
            self._journal.set_int("last_transport_sequence", transport_sequence)

        envelope = MarketDataEnvelope(
            run_id=self.run_id,
            transport_sequence=transport_sequence,
            channel_id=channel_id,
            source_sequence=source_sequence,
            sim_time_ns=int(sim_time_ns),
            generated_wall_time_ns=now_ns,
            event_type=event_type,
            symbol=symbol,
            payload=dict(payload),
        )
        frame = encode_message(
            market_data_message(envelope),
            compression_threshold=self.config.compression_threshold,
        )
        self._journal.append(
            transport_sequence=transport_sequence,
            channel_id=channel_id,
            source_sequence=source_sequence,
            frame=frame,
            created_wall_time_ns=now_ns,
        )
        with self._condition:
            self._condition.notify_all()
        return envelope

    def _next_source_sequence(self, channel_id: str) -> int:
        with self._sequence_lock:
            current = self._source_sequences.get(channel_id)
            if current is None:
                current = self._journal.source_sequence(channel_id)
            current += 1
            self._source_sequences[channel_id] = current
            self._journal.set_source_sequence(channel_id, current)
            return current

    def _current_source_sequence(self, channel_id: str) -> int:
        with self._sequence_lock:
            current = self._source_sequences.get(channel_id)
            if current is None:
                current = self._journal.source_sequence(channel_id)
                self._source_sequences[channel_id] = current
            return current

    def _wait_for_capacity(self) -> None:
        deadline = time.monotonic() + self.config.pending_wait_timeout_seconds
        while self._journal.pending_count() >= self.config.max_pending_events:
            if self.config.pending_policy == "fail":
                raise EmitterBackpressureError(
                    "emitter outbox reached max_pending_events; no event was dropped"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise EmitterBackpressureError(
                    "timed out waiting for emitter outbox capacity"
                )
            with self._condition:
                self._condition.wait(timeout=min(remaining, 0.1))

    def _publisher_loop(self) -> None:
        socket: zmq.Socket | None = None
        try:
            socket = self._context.socket(zmq.DEALER)
            identity = self.config.identity or f"{self.run_id}-{os.getpid()}-{uuid.uuid4()}"
            socket.setsockopt(zmq.IDENTITY, identity.encode("utf-8"))
            socket.setsockopt(zmq.LINGER, self.config.linger_ms)
            socket.setsockopt(zmq.SNDHWM, self.config.send_hwm)
            socket.setsockopt(zmq.RCVHWM, self.config.receive_hwm)
            socket.connect(self.config.endpoint)

            poller = zmq.Poller()
            poller.register(socket, zmq.POLLIN)
            ready = False
            inflight: OrderedDict[int, float] = OrderedDict()
            send_cursor = self._last_ack
            last_hello = 0.0

            while not self._stop_event.is_set():
                now = time.monotonic()
                if not ready and now - last_hello >= self.config.reconnect_interval_seconds:
                    hello = control_message(
                        "HELLO",
                        run_id=self.run_id,
                        last_acknowledged_transport_sequence=self._last_ack,
                    )
                    socket.send(encode_message(hello))
                    last_hello = now

                events = dict(poller.poll(timeout=50))
                if socket in events:
                    message = decode_message(socket.recv())
                    kind = message["kind"]
                    body = message["body"]
                    if kind == "READY":
                        if body.get("run_id") != self.run_id:
                            raise RuntimeError("READY run_id does not match emitter run_id")
                        watermark = int(body.get("last_persisted_transport_sequence", 0))
                        self._acknowledge(watermark)
                        inflight = OrderedDict(
                            (sequence, sent_at)
                            for sequence, sent_at in inflight.items()
                            if sequence > watermark
                        )
                        send_cursor = watermark
                        ready = True
                    elif kind == "ACK":
                        watermark = int(body["last_persisted_transport_sequence"])
                        self._acknowledge(watermark)
                        inflight = OrderedDict(
                            (sequence, sent_at)
                            for sequence, sent_at in inflight.items()
                            if sequence > watermark
                        )
                        send_cursor = max(send_cursor, watermark)
                    elif kind == "NACK":
                        logger.error("pacing server rejected message: %s", body)
                        ready = False
                        inflight.clear()
                        send_cursor = self._last_ack

                if ready:
                    for sequence, frame in self._journal.iter_unacknowledged(
                        after=send_cursor
                    ):
                        if len(inflight) >= self.config.max_inflight:
                            break
                        socket.send(frame)
                        inflight[sequence] = time.monotonic()
                        send_cursor = sequence

                    if inflight:
                        oldest_sequence, oldest_sent_at = next(iter(inflight.items()))
                        if time.monotonic() - oldest_sent_at >= self.config.ack_timeout_seconds:
                            logger.warning(
                                "ACK timeout at transport_sequence=%d; replaying unacknowledged outbox",
                                oldest_sequence,
                            )
                            ready = False
                            inflight.clear()
                            send_cursor = self._last_ack

                if not inflight and self._journal.pending_count() == 0:
                    with self._condition:
                        self._condition.wait(timeout=0.05)
        except BaseException as exc:  # surfaced through flush()/last_error
            self._last_error = exc
            logger.exception("market-data publisher thread failed")
        finally:
            if socket is not None:
                socket.close(linger=self.config.linger_ms)
            with self._condition:
                self._condition.notify_all()

    def _acknowledge(self, watermark: int) -> None:
        if watermark <= self._last_ack:
            return
        self._journal.mark_acknowledged_through(watermark)
        self._journal.set_int("last_acknowledged_sequence", watermark)
        if self.config.delete_acked_events:
            self._journal.delete_acknowledged_through(watermark)
        with self._condition:
            self._last_ack = watermark
            self._condition.notify_all()
