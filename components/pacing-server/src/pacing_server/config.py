from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(slots=True)
class PacingServerConfig:
    ingress_endpoint: str = "tcp://127.0.0.1:5557"
    egress_endpoint: str = "tcp://127.0.0.1:5558"
    control_endpoint: str = "tcp://127.0.0.1:5559"
    journal_path: Path = Path("./state/pacing-server.sqlite3")
    playback_speed: float = 1.0
    start_paused: bool = False
    ingress_hwm: int = 100_000
    egress_hwm: int = 100_000
    delivery_queue_capacity: int = 100_000
    linger_ms: int = 5_000
    send_timeout_ms: int = 250
    compression_threshold: int = 64 * 1024
    allow_multiple_runs: bool = False

    def validate(self) -> None:
        if self.playback_speed <= 0:
            raise ValueError("playback_speed must be positive")
        if self.ingress_hwm <= 0 or self.egress_hwm <= 0:
            raise ValueError("HWM values must be positive")
        if self.delivery_queue_capacity <= 0:
            raise ValueError("delivery_queue_capacity must be positive")
