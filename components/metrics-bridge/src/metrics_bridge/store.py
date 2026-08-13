from __future__ import annotations

import base64
import json
import threading
from collections import deque
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, Literal


ReplayMode = Literal["latest", "all", "none"]


def metric_event_id(metric: Mapping[str, Any]) -> str:
    encoded_run = base64.urlsafe_b64encode(metric["run_id"].encode("utf-8"))
    return f"{encoded_run.decode('ascii').rstrip('=')}.{metric['transport_sequence']}"


@dataclass(frozen=True, slots=True)
class PublishedMetric:
    event_id: str
    metric: dict[str, Any]
    json_data: str


@dataclass(frozen=True, slots=True)
class GapNotice:
    dropped_events: int
    latest_event_id: str


class Subscriber:
    def __init__(self, *, symbol: str | None, capacity: int) -> None:
        self.symbol = symbol
        self.capacity = capacity
        self._events: deque[PublishedMetric] = deque()
        self._condition = threading.Condition()
        self._closed = False
        self._dropped_events = 0
        self._latest_dropped_event_id = ""

    def accepts(self, event: PublishedMetric) -> bool:
        return self.symbol is None or event.metric["symbol"] == self.symbol

    def push(self, event: PublishedMetric) -> None:
        if not self.accepts(event):
            return
        with self._condition:
            if self._closed:
                return
            if len(self._events) >= self.capacity:
                dropped = self._events.popleft()
                self._dropped_events += 1
                self._latest_dropped_event_id = dropped.event_id
            self._events.append(event)
            self._condition.notify()

    def get(self, timeout: float) -> PublishedMetric | GapNotice | None:
        with self._condition:
            if not self._closed and not self._events and self._dropped_events == 0:
                self._condition.wait(timeout=timeout)
            if self._dropped_events:
                notice = GapNotice(
                    dropped_events=self._dropped_events,
                    latest_event_id=self._latest_dropped_event_id,
                )
                self._dropped_events = 0
                self._latest_dropped_event_id = ""
                return notice
            if self._events:
                return self._events.popleft()
            return None

    def close(self) -> None:
        with self._condition:
            self._closed = True
            self._condition.notify_all()


@dataclass(frozen=True, slots=True)
class Subscription:
    subscriber: Subscriber
    backlog: tuple[PublishedMetric, ...]
    reset: dict[str, Any] | None


class MetricStore:
    def __init__(self, *, history_size: int = 10_000, client_queue_size: int = 1_000) -> None:
        if history_size <= 0:
            raise ValueError("history_size must be positive")
        if client_queue_size <= 0:
            raise ValueError("client_queue_size must be positive")
        self._history: deque[PublishedMetric] = deque(maxlen=history_size)
        self._latest: dict[str, PublishedMetric] = {}
        self._subscribers: set[Subscriber] = set()
        self._client_queue_size = client_queue_size
        self._lock = threading.RLock()
        self._records_received = 0
        self._invalid_records = 0
        self._source_eof = False

    def publish(self, metric: Mapping[str, Any]) -> PublishedMetric:
        copied = dict(metric)
        event = PublishedMetric(
            event_id=metric_event_id(copied),
            metric=copied,
            json_data=json.dumps(
                copied,
                ensure_ascii=False,
                separators=(",", ":"),
                allow_nan=False,
            ),
        )
        with self._lock:
            self._history.append(event)
            self._latest[copied["symbol"]] = event
            self._records_received += 1
            subscribers = tuple(self._subscribers)
        for subscriber in subscribers:
            subscriber.push(event)
        return event

    def record_invalid(self) -> None:
        with self._lock:
            self._invalid_records += 1

    def mark_source_eof(self) -> None:
        with self._lock:
            self._source_eof = True

    def latest(self, symbol: str | None = None) -> list[PublishedMetric]:
        with self._lock:
            if symbol is not None:
                event = self._latest.get(symbol)
                return [] if event is None else [event]
            return sorted(
                self._latest.values(),
                key=lambda event: (event.metric["run_id"], event.metric["transport_sequence"]),
            )

    def subscribe(
        self,
        *,
        symbol: str | None = None,
        last_event_id: str | None = None,
        replay: ReplayMode = "latest",
    ) -> Subscription:
        if replay not in ("latest", "all", "none"):
            raise ValueError("replay must be latest, all, or none")
        subscriber = Subscriber(symbol=symbol, capacity=self._client_queue_size)
        with self._lock:
            history = [
                event
                for event in self._history
                if symbol is None or event.metric["symbol"] == symbol
            ]
            reset: dict[str, Any] | None = None
            if last_event_id:
                matching_index = next(
                    (
                        index
                        for index, event in enumerate(history)
                        if event.event_id == last_event_id
                    ),
                    None,
                )
                if matching_index is None:
                    backlog = history
                    reset = {
                        "reason": "replay_window_exceeded",
                        "last_event_id": last_event_id,
                        "oldest_available_event_id": (
                            history[0].event_id if history else None
                        ),
                        "latest_available_event_id": (
                            history[-1].event_id if history else None
                        ),
                    }
                else:
                    backlog = history[matching_index + 1 :]
            elif replay == "all":
                backlog = history
            elif replay == "latest":
                if symbol is not None:
                    backlog = history[-1:]
                else:
                    backlog = sorted(
                        self._latest.values(),
                        key=lambda event: (
                            event.metric["run_id"],
                            event.metric["transport_sequence"],
                        ),
                    )
            else:
                backlog = []
            self._subscribers.add(subscriber)
        return Subscription(subscriber, tuple(backlog), reset)

    def unsubscribe(self, subscriber: Subscriber) -> None:
        with self._lock:
            self._subscribers.discard(subscriber)
        subscriber.close()

    def stats(self) -> dict[str, Any]:
        with self._lock:
            latest_event = self._history[-1] if self._history else None
            return {
                "records_received": self._records_received,
                "invalid_records": self._invalid_records,
                "connected_clients": len(self._subscribers),
                "history_records": len(self._history),
                "source_eof": self._source_eof,
                "latest_event_id": (
                    latest_event.event_id if latest_event is not None else None
                ),
            }
