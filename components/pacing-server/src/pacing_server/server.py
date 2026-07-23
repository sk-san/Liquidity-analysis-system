from __future__ import annotations

import logging
import queue
import threading
import time
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any

import zmq

from market_data_protocol import (
    MarketDataEnvelope,
    WireDecodeError,
    control_message,
    decode_message,
    encode_message,
)

from .clock import PacingClock
from .config import PacingServerConfig
from .journal import ServerJournal
from .metrics import ServerMetrics

logger = logging.getLogger(__name__)


@dataclass(order=True, slots=True)
class DeliveryItem:
    # Transport order is the authoritative ABIDES event order. sim_time_ns is
    # used only by the pacing clock and must never reorder equal-time events.
    transport_sequence: int
    sim_time_ns: int
    run_id: str
    frame: bytes


class PacingServer:
    """Durable record-and-replay gateway between ABIDES and a calculation engine."""

    def __init__(
        self,
        config: PacingServerConfig | None = None,
        *,
        context: zmq.Context | None = None,
    ) -> None:
        self.config = config or PacingServerConfig()
        self.config.validate()
        self._context = context or zmq.Context.instance()
        self._journal = ServerJournal(self.config.journal_path)
        self._clock = PacingClock(
            speed=self.config.playback_speed,
            start_paused=self.config.start_paused,
        )
        self._metrics = ServerMetrics()
        self._stop_event = threading.Event()
        self._threads: list[threading.Thread] = []
        self._delivery_queue: queue.PriorityQueue[DeliveryItem] = queue.PriorityQueue(
            maxsize=self.config.delivery_queue_capacity
        )
        self._scheduled: set[tuple[str, int]] = set()
        self._scheduled_lock = threading.Lock()
        pending_runs = self._journal.undelivered_run_ids()
        self._active_run_id: str | None = pending_runs[0] if len(pending_runs) == 1 else None
        self._active_run_lock = threading.Lock()
        self._source_watermarks: dict[tuple[str, str], int] = (
            self._journal.latest_source_watermarks()
        )
        self._started = threading.Event()
        self._last_error: BaseException | None = None

    @property
    def last_error(self) -> BaseException | None:
        return self._last_error

    def start(self) -> None:
        if any(thread.is_alive() for thread in self._threads):
            return
        self._stop_event.clear()
        self._threads = [
            threading.Thread(target=self._ingress_loop, name="pacing-ingress", daemon=True),
            threading.Thread(target=self._feeder_loop, name="pacing-feeder", daemon=True),
            threading.Thread(target=self._delivery_loop, name="pacing-delivery", daemon=True),
            threading.Thread(target=self._control_loop, name="pacing-control", daemon=True),
        ]
        for thread in self._threads:
            thread.start()
        self._started.wait(timeout=2.0)

    def stop(self, *, timeout: float = 5.0) -> None:
        self._stop_event.set()
        self._clock.resume()
        for thread in self._threads:
            thread.join(timeout=timeout)
        self._journal.close()

    def __enter__(self) -> "PacingServer":
        self.start()
        return self

    def __exit__(self, exc_type: Any, exc: Any, tb: Any) -> None:
        self.stop()

    def stats(self) -> dict[str, Any]:
        total, delivered = self._journal.counts()
        result: dict[str, Any] = self._metrics.snapshot()
        result.update(
            {
                "journal_total": total,
                "journal_delivered": delivered,
                "delivery_queue_size": self._delivery_queue.qsize(),
                "active_run_id": self._active_run_id,
                "playback_speed": self._clock.speed,
                "paused": self._clock.paused,
            }
        )
        return result

    def _ingress_loop(self) -> None:
        socket: zmq.Socket | None = None
        try:
            socket = self._context.socket(zmq.ROUTER)
            socket.setsockopt(zmq.LINGER, self.config.linger_ms)
            socket.setsockopt(zmq.RCVHWM, self.config.ingress_hwm)
            socket.setsockopt(zmq.SNDHWM, self.config.ingress_hwm)
            socket.bind(self.config.ingress_endpoint)
            self._started.set()
            poller = zmq.Poller()
            poller.register(socket, zmq.POLLIN)
            while not self._stop_event.is_set():
                events = dict(poller.poll(timeout=100))
                if socket not in events:
                    continue
                frames = socket.recv_multipart()
                if len(frames) < 2:
                    continue
                identity, frame = frames[0], frames[-1]
                self._metrics.increment("received")
                try:
                    message = decode_message(frame)
                    kind = message["kind"]
                    body = message["body"]
                    if kind == "HELLO":
                        self._handle_hello(socket, identity, body)
                    elif kind == "MARKET_DATA":
                        self._handle_market_data(socket, identity, frame, body)
                    else:
                        self._send_control(
                            socket,
                            identity,
                            "NACK",
                            reason=f"unsupported ingress kind={kind}",
                        )
                except WireDecodeError as exc:
                    self._metrics.increment("decode_errors")
                    self._send_control(socket, identity, "NACK", reason=str(exc))
        except BaseException as exc:
            self._last_error = exc
            logger.exception("pacing ingress failed")
            self._stop_event.set()
        finally:
            if socket is not None:
                socket.close(linger=self.config.linger_ms)

    def _handle_hello(
        self, socket: zmq.Socket, identity: bytes, body: Mapping[str, Any]
    ) -> None:
        run_id = str(body.get("run_id", ""))
        if not run_id:
            self._send_control(socket, identity, "NACK", reason="HELLO missing run_id")
            return
        with self._active_run_lock:
            if self._active_run_id is None:
                self._active_run_id = run_id
            elif not self.config.allow_multiple_runs and self._active_run_id != run_id:
                self._send_control(
                    socket,
                    identity,
                    "NACK",
                    reason=f"server already owns active run {self._active_run_id}",
                )
                return
        watermark = self._journal.last_persisted_transport_sequence(run_id)
        self._send_control(
            socket,
            identity,
            "READY",
            run_id=run_id,
            last_persisted_transport_sequence=watermark,
        )

    def _handle_market_data(
        self,
        socket: zmq.Socket,
        identity: bytes,
        frame: bytes,
        body: Mapping[str, Any],
    ) -> None:
        envelope = MarketDataEnvelope.from_dict(body)
        with self._active_run_lock:
            if self._active_run_id is None:
                self._active_run_id = envelope.run_id
            elif (
                not self.config.allow_multiple_runs
                and self._active_run_id != envelope.run_id
            ):
                self._send_control(
                    socket,
                    identity,
                    "NACK",
                    reason=f"unexpected run_id={envelope.run_id}",
                )
                return

        inserted = self._journal.append_if_absent(
            run_id=envelope.run_id,
            transport_sequence=envelope.transport_sequence,
            channel_id=envelope.channel_id,
            source_sequence=envelope.source_sequence,
            sim_time_ns=envelope.sim_time_ns,
            event_type=envelope.event_type,
            frame=frame,
            received_wall_time_ns=time.time_ns(),
        )
        if inserted:
            self._metrics.increment("persisted")
            self._check_source_sequence(envelope)
        else:
            self._metrics.increment("duplicates")
        watermark = self._journal.last_persisted_transport_sequence(envelope.run_id)
        self._metrics.set_value("last_transport_sequence", watermark)
        self._send_control(
            socket,
            identity,
            "ACK",
            run_id=envelope.run_id,
            last_persisted_transport_sequence=watermark,
        )

    def _check_source_sequence(self, envelope: MarketDataEnvelope) -> None:
        key = (envelope.run_id, envelope.channel_id)
        if envelope.is_snapshot:
            self._source_watermarks[key] = envelope.source_sequence
            return
        if not envelope.is_incremental:
            return
        previous = self._source_watermarks.get(key)
        if previous is not None and envelope.source_sequence != previous + 1:
            self._metrics.increment("source_gaps")
            logger.error(
                "source sequence gap run=%s channel=%s expected=%d received=%d",
                envelope.run_id,
                envelope.channel_id,
                previous + 1,
                envelope.source_sequence,
            )
        self._source_watermarks[key] = envelope.source_sequence

    def _send_control(
        self, socket: zmq.Socket, identity: bytes, kind: str, **body: Any
    ) -> None:
        socket.send_multipart(
            [
                identity,
                encode_message(
                    control_message(kind, **body),
                    compression_threshold=self.config.compression_threshold,
                ),
            ]
        )

    def _feeder_loop(self) -> None:
        try:
            while not self._stop_event.is_set():
                with self._active_run_lock:
                    active_run_id = self._active_run_id
                if active_run_id is None:
                    self._stop_event.wait(0.05)
                    continue
                added = 0
                for run_id, sequence, sim_time_ns, frame in self._journal.iter_undelivered(
                    run_id=active_run_id,
                    limit=min(10_000, self.config.delivery_queue_capacity),
                ):
                    key = (run_id, sequence)
                    with self._scheduled_lock:
                        if key in self._scheduled:
                            continue
                        self._scheduled.add(key)
                    try:
                        self._delivery_queue.put(
                            DeliveryItem(sequence, sim_time_ns, run_id, frame),
                            timeout=0.1,
                        )
                        added += 1
                    except queue.Full:
                        with self._scheduled_lock:
                            self._scheduled.discard(key)
                        break
                if added == 0:
                    self._stop_event.wait(0.05)
        except BaseException as exc:
            self._last_error = exc
            logger.exception("pacing feeder failed")
            self._stop_event.set()

    def _delivery_loop(self) -> None:
        socket: zmq.Socket | None = None
        try:
            socket = self._context.socket(zmq.PUSH)
            socket.setsockopt(zmq.LINGER, self.config.linger_ms)
            socket.setsockopt(zmq.SNDHWM, self.config.egress_hwm)
            socket.setsockopt(zmq.SNDTIMEO, self.config.send_timeout_ms)
            # Do not report a send as successful until at least one PULL peer is
            # actually connected. This prevents journal rows from being marked
            # delivered while the calculation engine is absent.
            socket.setsockopt(zmq.IMMEDIATE, 1)
            socket.bind(self.config.egress_endpoint)
            while not self._stop_event.is_set():
                try:
                    item = self._delivery_queue.get(timeout=0.1)
                except queue.Empty:
                    continue
                try:
                    message = decode_message(item.frame)
                    envelope = MarketDataEnvelope.from_dict(message["body"])
                    lateness_ns = self._clock.wait_until(
                        envelope.sim_time_ns, self._stop_event
                    )
                    self._metrics.set_max("maximum_lateness_ns", lateness_ns)
                    outbound = dict(message)
                    outbound_body = dict(outbound["body"])
                    outbound_body["pacing_delivery_wall_time_ns"] = time.time_ns()
                    outbound_body["pacing_lateness_ns"] = lateness_ns
                    outbound_body["playback_speed"] = self._clock.speed
                    outbound["body"] = outbound_body
                    output_frame = encode_message(
                        outbound,
                        compression_threshold=self.config.compression_threshold,
                    )
                    while not self._stop_event.is_set():
                        try:
                            socket.send(output_frame)
                            break
                        except zmq.Again:
                            continue
                    if self._stop_event.is_set():
                        continue
                    delivered_at = time.time_ns()
                    self._journal.mark_delivered(
                        run_id=item.run_id,
                        transport_sequence=item.transport_sequence,
                        delivered_wall_time_ns=delivered_at,
                    )
                    self._metrics.increment("delivered")
                    self._metrics.set_value(
                        "last_delivered_transport_sequence", item.transport_sequence
                    )
                finally:
                    with self._scheduled_lock:
                        self._scheduled.discard((item.run_id, item.transport_sequence))
                    self._delivery_queue.task_done()
        except BaseException as exc:
            self._last_error = exc
            logger.exception("pacing delivery failed")
            self._stop_event.set()
        finally:
            if socket is not None:
                socket.close(linger=self.config.linger_ms)

    def _control_loop(self) -> None:
        socket: zmq.Socket | None = None
        try:
            socket = self._context.socket(zmq.REP)
            socket.setsockopt(zmq.LINGER, 0)
            socket.bind(self.config.control_endpoint)
            poller = zmq.Poller()
            poller.register(socket, zmq.POLLIN)
            while not self._stop_event.is_set():
                events = dict(poller.poll(timeout=100))
                if socket not in events:
                    continue
                try:
                    request = decode_message(socket.recv())
                    kind = request["kind"]
                    body = request["body"]
                    if kind != "CONTROL":
                        raise ValueError("control endpoint expects kind=CONTROL")
                    command = str(body.get("command", "")).lower()
                    response = self._handle_control(command, body)
                    socket.send(encode_message(control_message("CONTROL_RESULT", **response)))
                except Exception as exc:
                    socket.send(
                        encode_message(
                            control_message("CONTROL_RESULT", ok=False, error=str(exc))
                        )
                    )
        except BaseException as exc:
            self._last_error = exc
            logger.exception("pacing control failed")
            self._stop_event.set()
        finally:
            if socket is not None:
                socket.close(linger=0)

    def _handle_control(self, command: str, body: Mapping[str, Any]) -> dict[str, Any]:
        if command == "stats":
            return {"ok": True, "stats": self.stats()}
        if command == "pause":
            self._clock.pause()
            return {"ok": True}
        if command == "resume":
            self._clock.resume()
            return {"ok": True}
        if command == "set_speed":
            self._clock.set_speed(float(body["speed"]))
            return {"ok": True, "speed": self._clock.speed}
        if command == "latest_snapshot":
            frame = self._journal.latest_snapshot(
                run_id=str(body["run_id"]), channel_id=str(body["channel_id"])
            )
            return {"ok": frame is not None, "frame": frame}
        if command == "stop":
            self._stop_event.set()
            return {"ok": True}
        raise ValueError(f"unsupported control command={command}")
