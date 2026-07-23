import threading
import time

from pacing_server import PacingClock


def test_clock_preserves_relative_time() -> None:
    clock = PacingClock(speed=10.0)
    stop = threading.Event()
    start = time.monotonic()
    clock.wait_until(1_000_000_000, stop)
    clock.wait_until(1_100_000_000, stop)
    elapsed = time.monotonic() - start
    assert elapsed >= 0.008
    assert elapsed < 0.1


def test_pause_and_resume() -> None:
    clock = PacingClock(speed=100.0, start_paused=True)
    stop = threading.Event()
    finished = threading.Event()

    def worker() -> None:
        clock.wait_until(1_000, stop)
        finished.set()

    thread = threading.Thread(target=worker)
    thread.start()
    time.sleep(0.02)
    assert not finished.is_set()
    clock.resume()
    assert finished.wait(0.5)
    thread.join()
