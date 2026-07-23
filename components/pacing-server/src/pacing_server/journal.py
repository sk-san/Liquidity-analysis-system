from __future__ import annotations

import sqlite3
import threading
from collections.abc import Iterator
from pathlib import Path


class ServerJournal:
    """Durable inbound journal; ACK is sent only after append succeeds."""

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
                    run_id TEXT NOT NULL,
                    transport_sequence INTEGER NOT NULL,
                    channel_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    sim_time_ns INTEGER NOT NULL,
                    event_type TEXT NOT NULL,
                    frame BLOB NOT NULL,
                    received_wall_time_ns INTEGER NOT NULL,
                    delivered INTEGER NOT NULL DEFAULT 0,
                    delivered_wall_time_ns INTEGER,
                    PRIMARY KEY(run_id, transport_sequence)
                );
                CREATE INDEX IF NOT EXISTS events_delivery_idx
                    ON events(delivered, run_id, sim_time_ns, transport_sequence);
                CREATE TABLE IF NOT EXISTS snapshots (
                    run_id TEXT NOT NULL,
                    channel_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    transport_sequence INTEGER NOT NULL,
                    frame BLOB NOT NULL,
                    PRIMARY KEY(run_id, channel_id)
                );
                CREATE TABLE IF NOT EXISTS run_state (
                    run_id TEXT PRIMARY KEY,
                    last_contiguous_transport_sequence INTEGER NOT NULL DEFAULT 0
                );
                """
            )

    def close(self) -> None:
        with self._lock:
            self._conn.close()

    def append_if_absent(
        self,
        *,
        run_id: str,
        transport_sequence: int,
        channel_id: str,
        source_sequence: int,
        sim_time_ns: int,
        event_type: str,
        frame: bytes,
        received_wall_time_ns: int,
    ) -> bool:
        with self._lock:
            cursor = self._conn.execute(
                """
                INSERT OR IGNORE INTO events(
                    run_id, transport_sequence, channel_id, source_sequence,
                    sim_time_ns, event_type, frame, received_wall_time_ns
                ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    run_id,
                    transport_sequence,
                    channel_id,
                    source_sequence,
                    sim_time_ns,
                    event_type,
                    sqlite3.Binary(frame),
                    received_wall_time_ns,
                ),
            )
            inserted = cursor.rowcount == 1
            if inserted:
                self._advance_contiguous_watermark_locked(run_id)
            if inserted and event_type == "SNAPSHOT":
                self._conn.execute(
                    """
                    INSERT INTO snapshots(
                        run_id, channel_id, source_sequence,
                        transport_sequence, frame
                    ) VALUES (?, ?, ?, ?, ?)
                    ON CONFLICT(run_id, channel_id) DO UPDATE SET
                        source_sequence = excluded.source_sequence,
                        transport_sequence = excluded.transport_sequence,
                        frame = excluded.frame
                    """,
                    (
                        run_id,
                        channel_id,
                        source_sequence,
                        transport_sequence,
                        sqlite3.Binary(frame),
                    ),
                )
            return inserted

    def last_persisted_transport_sequence(self, run_id: str) -> int:
        """Return the highest contiguous transport sequence durably present."""
        with self._lock:
            row = self._conn.execute(
                """
                SELECT last_contiguous_transport_sequence
                FROM run_state WHERE run_id = ?
                """,
                (run_id,),
            ).fetchone()
            return int(row[0]) if row else 0

    def _advance_contiguous_watermark_locked(self, run_id: str) -> None:
        row = self._conn.execute(
            """
            SELECT last_contiguous_transport_sequence
            FROM run_state WHERE run_id = ?
            """,
            (run_id,),
        ).fetchone()
        watermark = int(row[0]) if row else 0
        while True:
            next_row = self._conn.execute(
                """
                SELECT 1 FROM events
                WHERE run_id = ? AND transport_sequence = ?
                """,
                (run_id, watermark + 1),
            ).fetchone()
            if next_row is None:
                break
            watermark += 1
        self._conn.execute(
            """
            INSERT INTO run_state(run_id, last_contiguous_transport_sequence)
            VALUES (?, ?)
            ON CONFLICT(run_id) DO UPDATE SET
                last_contiguous_transport_sequence = excluded.last_contiguous_transport_sequence
            """,
            (run_id, watermark),
        )

    def iter_undelivered(
        self, *, run_id: str | None = None, limit: int = 10_000
    ) -> Iterator[tuple[str, int, int, bytes]]:
        with self._lock:
            if run_id is None:
                rows = list(
                    self._conn.execute(
                        """
                        SELECT run_id, transport_sequence, sim_time_ns, frame
                        FROM events WHERE delivered = 0
                        ORDER BY transport_sequence
                        LIMIT ?
                        """,
                        (limit,),
                    )
                )
            else:
                rows = list(
                    self._conn.execute(
                        """
                        SELECT run_id, transport_sequence, sim_time_ns, frame
                        FROM events WHERE delivered = 0 AND run_id = ?
                        ORDER BY transport_sequence
                        LIMIT ?
                        """,
                        (run_id, limit),
                    )
                )
        for item in rows:
            yield str(item[0]), int(item[1]), int(item[2]), bytes(item[3])

    def mark_delivered(
        self, *, run_id: str, transport_sequence: int, delivered_wall_time_ns: int
    ) -> None:
        with self._lock:
            self._conn.execute(
                """
                UPDATE events
                SET delivered = 1, delivered_wall_time_ns = ?
                WHERE run_id = ? AND transport_sequence = ?
                """,
                (delivered_wall_time_ns, run_id, transport_sequence),
            )

    def latest_snapshot(self, *, run_id: str, channel_id: str) -> bytes | None:
        with self._lock:
            row = self._conn.execute(
                """
                SELECT frame FROM snapshots
                WHERE run_id = ? AND channel_id = ?
                """,
                (run_id, channel_id),
            ).fetchone()
            return None if row is None else bytes(row[0])

    def latest_source_watermarks(self) -> dict[tuple[str, str], int]:
        with self._lock:
            rows = self._conn.execute(
                """
                SELECT e.run_id, e.channel_id, e.source_sequence
                FROM events e
                JOIN (
                    SELECT run_id, channel_id, MAX(transport_sequence) AS max_transport
                    FROM events
                    GROUP BY run_id, channel_id
                ) latest
                ON e.run_id = latest.run_id
                AND e.channel_id = latest.channel_id
                AND e.transport_sequence = latest.max_transport
                """
            ).fetchall()
            return {(str(r), str(c)): int(s) for r, c, s in rows}

    def undelivered_run_ids(self) -> list[str]:
        with self._lock:
            rows = self._conn.execute(
                "SELECT DISTINCT run_id FROM events WHERE delivered = 0 ORDER BY run_id"
            ).fetchall()
            return [str(row[0]) for row in rows]

    def counts(self) -> tuple[int, int]:
        with self._lock:
            row = self._conn.execute(
                """
                SELECT COUNT(*), SUM(CASE WHEN delivered = 1 THEN 1 ELSE 0 END)
                FROM events
                """
            ).fetchone()
            total = int(row[0] or 0)
            delivered = int(row[1] or 0)
            return total, delivered
