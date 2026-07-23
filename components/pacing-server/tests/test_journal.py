from pathlib import Path

from pacing_server.journal import ServerJournal


def test_ack_watermark_is_contiguous(tmp_path: Path) -> None:
    journal = ServerJournal(tmp_path / "server.sqlite3")
    common = dict(
        run_id="run-1",
        channel_id="TOPIX",
        source_sequence=1,
        sim_time_ns=100,
        event_type="ADD",
        frame=b"frame",
        received_wall_time_ns=200,
    )
    journal.append_if_absent(transport_sequence=1, **common)
    journal.append_if_absent(transport_sequence=3, **common)
    assert journal.last_persisted_transport_sequence("run-1") == 1
    journal.append_if_absent(transport_sequence=2, **common)
    assert journal.last_persisted_transport_sequence("run-1") == 3
    journal.close()
