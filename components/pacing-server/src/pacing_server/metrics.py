from __future__ import annotations

import threading
from dataclasses import asdict, dataclass


@dataclass(slots=True)
class MetricsSnapshot:
    received: int = 0
    persisted: int = 0
    duplicates: int = 0
    delivered: int = 0
    decode_errors: int = 0
    source_gaps: int = 0
    maximum_lateness_ns: int = 0
    last_transport_sequence: int = 0
    last_delivered_transport_sequence: int = 0


class ServerMetrics:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._values = MetricsSnapshot()

    def increment(self, name: str, amount: int = 1) -> None:
        with self._lock:
            setattr(self._values, name, getattr(self._values, name) + amount)

    def set_max(self, name: str, value: int) -> None:
        with self._lock:
            setattr(self._values, name, max(getattr(self._values, name), value))

    def set_value(self, name: str, value: int) -> None:
        with self._lock:
            setattr(self._values, name, value)

    def snapshot(self) -> dict[str, int]:
        with self._lock:
            return asdict(self._values)
