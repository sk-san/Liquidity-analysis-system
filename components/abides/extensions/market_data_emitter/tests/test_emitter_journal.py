from pathlib import Path

from abides_market_data_emitter import EmitterConfig, MarketDataEmitter


def _order() -> dict:
    return {
        "entry_id": 2,
        "order_id": 1,
        "agent_id": 42,
        "priority_time_ns": 100,
        "symbol": "TOPIX",
        "side": "BID",
        "price": 276450,
        "quantity": 10,
        "visibility": "VISIBLE",
        "insert_by_id": False,
    }


def test_sequences_are_durable(tmp_path: Path) -> None:
    path = tmp_path / "emitter.sqlite3"
    emitter = MarketDataEmitter(
        run_id="run-1",
        config=EmitterConfig(journal_path=path, endpoint="inproc://unused"),
    )
    first = emitter.emit_snapshot(
        channel_id="TOPIX",
        symbol="TOPIX",
        sim_time_ns=100,
        orders=[],
    )
    second = emitter.emit_incremental(
        channel_id="TOPIX",
        symbol="TOPIX",
        sim_time_ns=101,
        event_type="ADD",
        payload={"order": _order()},
    )
    assert first.transport_sequence == 1
    assert first.source_sequence == 0
    assert second.transport_sequence == 2
    assert second.source_sequence == 1
    emitter.close(flush=False)

    restored = MarketDataEmitter(
        run_id="run-1",
        config=EmitterConfig(journal_path=path, endpoint="inproc://unused"),
    )
    third = restored.emit_snapshot(
        channel_id="TOPIX",
        symbol="TOPIX",
        sim_time_ns=102,
        orders=[_order()],
    )
    assert third.transport_sequence == 3
    assert third.source_sequence == 1
    restored.close(flush=False)
