from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(slots=True)
class EmitterConfig:
    endpoint: str = "tcp://127.0.0.1:5557"
    journal_path: Path = Path("./state/abides-emitter.sqlite3")
    identity: str | None = None
    max_inflight: int = 2_048
    max_pending_events: int = 1_000_000
    pending_policy: str = "fail"  # fail or block
    pending_wait_timeout_seconds: float = 5.0
    ack_timeout_seconds: float = 2.0
    reconnect_interval_seconds: float = 0.5
    linger_ms: int = 5_000
    send_hwm: int = 100_000
    receive_hwm: int = 10_000
    compression_threshold: int = 64 * 1024
    delete_acked_events: bool = False

    def validate(self) -> None:
        if self.max_inflight <= 0:
            raise ValueError("max_inflight must be positive")
        if self.max_pending_events <= 0:
            raise ValueError("max_pending_events must be positive")
        if self.pending_policy not in {"fail", "block"}:
            raise ValueError("pending_policy must be 'fail' or 'block'")
        if self.ack_timeout_seconds <= 0:
            raise ValueError("ack_timeout_seconds must be positive")
