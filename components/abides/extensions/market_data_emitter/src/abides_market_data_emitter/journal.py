from __future__ import annotations

import sqlite3
import threading
from collections.abc import Iterator
from pathlib import Path


class EmitterJournal:
    """Durable local outbox used before a frame is acknowledged by the server."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.RLock()
        self._conn = sqlite3.connect(
            self.path, check_same_thread=False, isolation_level=None, timeout=30.0
        )
        with self._lock:
            self._conn.execute("PRAGMA journal_mode=WAL")
            self._conn.execute("PRAGMA synchronous=NORMAL")
            self._conn.executescript(
                """
                CREATE TABLE IF NOT EXISTS events (
                    transport_sequence INTEGER PRIMARY KEY,
                    channel_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    frame BLOB NOT NULL,
                    acknowledged INTEGER NOT NULL DEFAULT 0,
                    created_wall_time_ns INTEGER NOT NULL
                );
                CREATE INDEX IF NOT EXISTS events_unacked_idx
                    ON events(acknowledged, transport_sequence);
                CREATE TABLE IF NOT EXISTS metadata (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
                """
            )

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    def append(
        self,
        *,
        transport_sequence: int,
        channel_id: str,
        source_sequence: int,
        frame: bytes,
        created_wall_time_ns: int,
    ) -> None:
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO events(
                    transport_sequence, channel_id, source_sequence,
                    frame, acknowledged, created_wall_time_ns
                ) VALUES (?, ?, ?, ?, 0, ?)
                """,
                (
                    transport_sequence,
                    channel_id,
                    source_sequence,
                    sqlite3.Binary(frame),
                    created_wall_time_ns,
                ),
            )

    def iter_unacknowledged(self, *, after: int = 0) -> Iterator[tuple[int, bytes]]:
        with self._lock:
            rows = list(
                self._conn.execute(
                    """
                    SELECT transport_sequence, frame
                    FROM events
                    WHERE acknowledged = 0 AND transport_sequence > ?
                    ORDER BY transport_sequence
                    """,
                    (after,),
                )
            )
        for sequence, frame in rows:
            yield int(sequence), bytes(frame)

    def mark_acknowledged_through(self, transport_sequence: int) -> int:
        with self._lock:
            cursor = self._conn.execute(
                """
                UPDATE events
                SET acknowledged = 1
                WHERE acknowledged = 0 AND transport_sequence <= ?
                """,
                (transport_sequence,),
            )
            return int(cursor.rowcount)

    def delete_acknowledged_through(self, transport_sequence: int) -> int:
        with self._lock:
            cursor = self._conn.execute(
                "DELETE FROM events WHERE acknowledged = 1 AND transport_sequence <= ?",
                (transport_sequence,),
            )
            return int(cursor.rowcount)

    def last_transport_sequence(self) -> int:
        with self._lock:
            row = self._conn.execute(
                "SELECT COALESCE(MAX(transport_sequence), 0) FROM events"
            ).fetchone()
            persisted = int(row[0]) if row else 0
            metadata = self.get_int("last_transport_sequence", default=0)
            return max(persisted, metadata)

    def last_acknowledged_sequence(self) -> int:
        with self._lock:
            row = self._conn.execute(
                "SELECT COALESCE(MAX(transport_sequence), 0) FROM events WHERE acknowledged = 1"
            ).fetchone()
            persisted = int(row[0]) if row else 0
            return max(persisted, self.get_int("last_acknowledged_sequence", default=0))

    def pending_count(self) -> int:
        with self._lock:
            row = self._conn.execute(
                "SELECT COUNT(*) FROM events WHERE acknowledged = 0"
            ).fetchone()
            return int(row[0]) if row else 0

    def set_text(self, key: str, value: str) -> None:
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO metadata(key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value
                """,
                (key, value),
            )

    def get_text(self, key: str, *, default: str | None = None) -> str | None:
        with self._lock:
            row = self._conn.execute(
                "SELECT value FROM metadata WHERE key = ?", (key,)
            ).fetchone()
            return str(row[0]) if row else default

    def set_int(self, key: str, value: int) -> None:
        with self._lock:
            self._conn.execute(
                """
                INSERT INTO metadata(key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value
                """,
                (key, str(value)),
            )

    def get_int(self, key: str, *, default: int = 0) -> int:
        with self._lock:
            row = self._conn.execute(
                "SELECT value FROM metadata WHERE key = ?", (key,)
            ).fetchone()
            return int(row[0]) if row else default

    def source_sequence(self, channel_id: str) -> int:
        return self.get_int(f"source_sequence:{channel_id}", default=0)

    def set_source_sequence(self, channel_id: str, value: int) -> None:
        self.set_int(f"source_sequence:{channel_id}", value)
