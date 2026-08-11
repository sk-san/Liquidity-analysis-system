from market_data_protocol import (
    MarketDataEnvelope,
    ProtocolValidationError,
    decode_message,
    encode_message,
    market_data_message,
)


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
        "is_market_maker":False,
        "insert_by_id": False,
    }


def test_round_trip_with_compression() -> None:
    envelope = MarketDataEnvelope(
        run_id="run-1",
        transport_sequence=1,
        channel_id="TOPIX",
        source_sequence=1,
        sim_time_ns=100,
        generated_wall_time_ns=200,
        event_type="ADD",
        symbol="TOPIX",
        payload={"order": _order()},
    )
    frame = encode_message(market_data_message(envelope), compression_threshold=0)
    decoded = decode_message(frame)
    assert decoded["body"]["payload"]["order"]["price"] == 276450


def test_invalid_side_is_rejected() -> None:
    order = _order()
    order["side"] = "BUY"
    envelope = MarketDataEnvelope(
        run_id="run-1",
        transport_sequence=1,
        channel_id="TOPIX",
        source_sequence=1,
        sim_time_ns=100,
        generated_wall_time_ns=200,
        event_type="ADD",
        symbol="TOPIX",
        payload={"order": order},
    )
    try:
        envelope.validate()
    except ProtocolValidationError:
        pass
    else:
        raise AssertionError("invalid side was accepted")
