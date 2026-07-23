from __future__ import annotations

import threading
import time


class PacingClock:
    """Maps simulation nanoseconds to monotonic wall-clock time."""

    def __init__(self, *, speed: float = 1.0, start_paused: bool = False) -> None:
        if speed <= 0:
            raise ValueError("speed must be positive")
        self._speed = float(speed)
        self._paused = bool(start_paused)
        self._base_sim_ns: int | None = None
        self._base_wall = time.monotonic()
        self._paused_sim_ns: int | None = None
        self._condition = threading.Condition()

    @property
    def speed(self) -> float:
        with self._condition:
            return self._speed

    @property
    def paused(self) -> bool:
        with self._condition:
            return self._paused

    def reset(self) -> None:
        with self._condition:
            self._base_sim_ns = None
            self._base_wall = time.monotonic()
            self._paused_sim_ns = None
            self._condition.notify_all()

    def pause(self) -> None:
        with self._condition:
            if self._paused:
                return
            self._paused_sim_ns = self._virtual_sim_time_locked()
            self._paused = True
            self._condition.notify_all()

    def resume(self) -> None:
        with self._condition:
            if not self._paused:
                return
            if self._paused_sim_ns is not None:
                self._base_sim_ns = self._paused_sim_ns
            self._base_wall = time.monotonic()
            self._paused_sim_ns = None
            self._paused = False
            self._condition.notify_all()

    def set_speed(self, speed: float) -> None:
        if speed <= 0:
            raise ValueError("speed must be positive")
        with self._condition:
            current_sim = self._virtual_sim_time_locked()
            self._speed = float(speed)
            if current_sim is not None:
                self._base_sim_ns = current_sim
                self._base_wall = time.monotonic()
                if self._paused:
                    self._paused_sim_ns = current_sim
            self._condition.notify_all()

    def wait_until(self, sim_time_ns: int, stop_event: threading.Event) -> int:
        """Wait for a simulation timestamp and return lateness in nanoseconds."""

        with self._condition:
            if self._base_sim_ns is None:
                self._base_sim_ns = int(sim_time_ns)
                self._base_wall = time.monotonic()
                if self._paused:
                    self._paused_sim_ns = int(sim_time_ns)

            while not stop_event.is_set():
                if self._paused:
                    self._condition.wait(timeout=0.1)
                    continue
                target = self._base_wall + (
                    (int(sim_time_ns) - self._base_sim_ns) / 1_000_000_000 / self._speed
                )
                remaining = target - time.monotonic()
                if remaining <= 0:
                    return max(0, int(-remaining * 1_000_000_000))
                self._condition.wait(timeout=min(remaining, 0.1))
            return 0

    def _virtual_sim_time_locked(self) -> int | None:
        if self._base_sim_ns is None:
            return None
        if self._paused and self._paused_sim_ns is not None:
            return self._paused_sim_ns
        elapsed_ns = int((time.monotonic() - self._base_wall) * 1_000_000_000)
        return self._base_sim_ns + int(elapsed_ns * self._speed)
