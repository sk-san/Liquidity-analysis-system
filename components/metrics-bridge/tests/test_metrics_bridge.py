from __future__ import annotations

import http.client
import io
import json
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator

import pytest

from metrics_bridge.ingest import ingest_path, ingest_stream, ingest_zmq
from metrics_bridge.model import (
    METRIC_JSON_SCHEMA,
    MetricValidationError,
    parse_metric_line,
)
from metrics_bridge.server import MetricsBridgeServer
from metrics_bridge.store import GapNotice, MetricStore, PublishedMetric


def metric(*, sequence: int = 7, symbol: str = "ABM") -> dict[str, object]:
    return {
        "run_id": "run-1",
        "transport_sequence": sequence,
        "symbol": symbol,
        "sequence": sequence,
        "exchange_time_ns": 1_612_517_400_000_000_000 + sequence,
        "received_wall_time_ns": 1_786_041_733_922_396_000 + sequence,
        "synchronized": True,
        "order_count": 2,
        "best_bid_price": 99994,
        "best_bid_quantity": 100,
        "best_ask_price": 99995,
        "best_ask_quantity": 200,
        "quoted_spread": 1,
        "midprice": 99994.5,
        "microprice": 99994.33333333333,
        "top_of_book_imbalance": -0.3333333333333333,
        "bid_visible_depth": 100,
        "ask_visible_depth": 200,
        "trade_count": 0,
        "traded_volume": 0,
        "last_trade_price": None,
        "bid_liquidity_provision_ratio": None,
        "ask_liquidity_provision_ratio": 0,
        "visible_checksum": 1_991_653_312_153_916_401,
        "bid_levels": [
            {
                "price": 99994,
                "visible_quantity": 100,
                "visible_mm_quantity": 40,
                "visible_order_count": 2,
            }
        ],
        "ask_levels": [
            {
                "price": 99995,
                "visible_quantity": 200,
                "visible_mm_quantity": 120,
                "visible_order_count": 3,
            }
        ],
    }


@contextmanager
def running_server(
    store: MetricStore,
    ui_dir: Path | None = None,
) -> Iterator[tuple[MetricsBridgeServer, threading.Thread]]:
    server = MetricsBridgeServer(
        port=0,
        store=store,
        heartbeat_seconds=0.1,
        ui_dir=ui_dir,
    )
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server, thread
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def test_parses_reference_metric_shape() -> None:
    expected = metric()
    assert parse_metric_line(json.dumps(expected)) == expected


def test_order_book_levels_are_optional_for_legacy_records() -> None:
    legacy = metric()
    del legacy["bid_levels"]
    del legacy["ask_levels"]

    assert parse_metric_line(json.dumps(legacy)) == legacy
    assert "bid_levels" not in METRIC_JSON_SCHEMA["required"]
    assert "ask_levels" not in METRIC_JSON_SCHEMA["required"]


def test_order_book_level_arrays_may_be_empty() -> None:
    expected = metric()
    expected["bid_levels"] = []
    expected["ask_levels"] = []

    assert parse_metric_line(json.dumps(expected)) == expected


def test_schema_describes_nested_order_book_levels() -> None:
    for field in ("bid_levels", "ask_levels"):
        levels_schema = METRIC_JSON_SCHEMA["properties"][field]
        assert levels_schema["type"] == "array"
        level_schema = levels_schema["items"]
        assert set(level_schema["required"]) == {
            "price",
            "visible_quantity",
            "visible_mm_quantity",
        }
        assert all(
            level_schema["properties"][name] == {
                "type": "integer",
                "minimum": 0,
            }
            for name in level_schema["required"]
        )
        assert level_schema["properties"]["visible_order_count"] == {
            "type": "integer",
            "minimum": 0,
        }


@pytest.mark.parametrize(
    ("field", "levels", "message"),
    [
        ("bid_levels", None, "bid_levels must be an array"),
        ("ask_levels", {}, "ask_levels must be an array"),
        ("bid_levels", ["not-a-level"], r"bid_levels\[0\] must be an object"),
        (
            "bid_levels",
            [{"visible_quantity": 1, "visible_mm_quantity": 0}],
            r"bid_levels\[0\] missing fields",
        ),
        (
            "bid_levels",
            [{"price": True, "visible_quantity": 1, "visible_mm_quantity": 0}],
            r"bid_levels\[0\]\.price must be an integer",
        ),
        (
            "ask_levels",
            [{"price": -1, "visible_quantity": 1, "visible_mm_quantity": 0}],
            r"ask_levels\[0\]\.price must not be negative",
        ),
        (
            "bid_levels",
            [{"price": 1, "visible_quantity": -1, "visible_mm_quantity": 0}],
            r"bid_levels\[0\]\.visible_quantity must not be negative",
        ),
        (
            "ask_levels",
            [{"price": 1, "visible_quantity": 1, "visible_mm_quantity": -1}],
            r"ask_levels\[0\]\.visible_mm_quantity must not be negative",
        ),
        (
            "bid_levels",
            [{"price": 1, "visible_quantity": 2, "visible_mm_quantity": 3}],
            r"bid_levels\[0\]\.visible_mm_quantity must not exceed",
        ),
        (
            "bid_levels",
            [
                {
                    "price": 1,
                    "visible_quantity": 2,
                    "visible_mm_quantity": 1,
                    "visible_order_count": True,
                }
            ],
            r"bid_levels\[0\]\.visible_order_count must be an integer",
        ),
    ],
)
def test_rejects_malformed_order_book_levels(
    field: str,
    levels: object,
    message: str,
) -> None:
    invalid = metric()
    invalid[field] = levels

    with pytest.raises(MetricValidationError, match=message):
        parse_metric_line(json.dumps(invalid))


def test_rejects_missing_and_non_finite_values() -> None:
    missing = metric()
    del missing["midprice"]
    with pytest.raises(MetricValidationError, match="missing fields"):
        parse_metric_line(json.dumps(missing))

    non_finite = metric()
    non_finite["midprice"] = float("nan")
    with pytest.raises(MetricValidationError, match="non-finite"):
        parse_metric_line(json.dumps(non_finite))


def test_ingestion_discards_bad_lines_and_keeps_processing() -> None:
    store = MetricStore()
    stream = io.StringIO(
        "not-json\n" + json.dumps(metric(sequence=8)) + "\n"
    )
    ingest_stream(stream, store, stop_event=threading.Event())

    assert store.latest("ABM")[0].metric["transport_sequence"] == 8
    assert store.stats() == {
        "records_received": 1,
        "invalid_records": 1,
        "connected_clients": 0,
        "history_records": 1,
        "source_eof": True,
        "latest_event_id": store.latest("ABM")[0].event_id,
    }


def test_follow_waits_for_a_complete_ndjson_line(tmp_path: Path) -> None:
    store = MetricStore()
    stop_event = threading.Event()
    serialized = json.dumps(metric(sequence=9))
    split = len(serialized) // 2
    path = tmp_path / "metrics.ndjson"
    path.write_text(serialized[:split], encoding="utf-8")
    thread = threading.Thread(
        target=ingest_path,
        kwargs={
            "path": path,
            "store": store,
            "stop_event": stop_event,
            "follow": True,
            "poll_interval": 0.01,
        },
    )
    thread.start()
    time.sleep(0.05)
    assert store.stats()["records_received"] == 0

    with path.open("a", encoding="utf-8") as output:
        output.write(serialized[split:] + "\n")
        output.flush()
    deadline = time.monotonic() + 1
    while store.stats()["records_received"] == 0 and time.monotonic() < deadline:
        time.sleep(0.01)

    stop_event.set()
    thread.join(timeout=1)
    assert store.latest("ABM")[0].metric["transport_sequence"] == 9
    assert store.stats()["invalid_records"] == 0


def test_ingest_zmq_receives_validates_and_archives(tmp_path: Path) -> None:
    zmq = pytest.importorskip("zmq")
    store = MetricStore()
    stop_event = threading.Event()
    endpoint = "inproc://metrics-bridge-test"
    archive_path = tmp_path / "archive.ndjson"

    with archive_path.open("a", encoding="utf-8") as archive:
        thread = threading.Thread(
            target=ingest_zmq,
            kwargs={
                "endpoint": endpoint,
                "store": store,
                "stop_event": stop_event,
                "poll_interval": 0.01,
                "archive": archive,
            },
            daemon=True,
        )
        thread.start()

        push = zmq.Context.instance().socket(zmq.PUSH)
        push.connect(endpoint)
        try:
            serialized = json.dumps(metric(sequence=21))
            push.send(serialized.encode("utf-8"))
            push.send(b"not json")

            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                stats = store.stats()
                if stats["records_received"] == 1 and stats["invalid_records"] == 1:
                    break
                time.sleep(0.01)
        finally:
            push.close(linger=0)
            stop_event.set()
            thread.join(timeout=2)

    assert store.latest("ABM")[0].metric["transport_sequence"] == 21
    stats = store.stats()
    assert stats["records_received"] == 1
    assert stats["invalid_records"] == 1
    assert stats["source_eof"] is True
    assert archive_path.read_text(encoding="utf-8") == serialized + "\n"


def test_reconnect_replays_only_events_after_last_event_id() -> None:
    store = MetricStore(history_size=4)
    first = store.publish(metric(sequence=1))
    second = store.publish(metric(sequence=2))
    third = store.publish(metric(sequence=3))

    subscription = store.subscribe(last_event_id=first.event_id)
    assert [event.event_id for event in subscription.backlog] == [
        second.event_id,
        third.event_id,
    ]
    assert subscription.reset is None
    store.unsubscribe(subscription.subscriber)


def test_slow_subscriber_gets_an_explicit_gap_notice() -> None:
    store = MetricStore(client_queue_size=1)
    subscription = store.subscribe(replay="none")
    store.publish(metric(sequence=1))
    store.publish(metric(sequence=2))

    notice = subscription.subscriber.get(timeout=0)
    assert isinstance(notice, GapNotice)
    assert notice.dropped_events == 1
    delivered = subscription.subscriber.get(timeout=0)
    assert isinstance(delivered, PublishedMetric)
    assert delivered.metric["transport_sequence"] == 2
    store.unsubscribe(subscription.subscriber)


def _get(
    host: str, port: int, path: str
) -> tuple[int, dict[str, str], bytes]:
    connection = http.client.HTTPConnection(host, port, timeout=2)
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        return (
            response.status,
            {key.lower(): value for key, value in response.getheaders()},
            response.read(),
        )
    finally:
        connection.close()


def test_serves_static_ui_when_configured(tmp_path: Path) -> None:
    ui_dir = tmp_path / "ui"
    (ui_dir / "assets").mkdir(parents=True)
    (ui_dir / "index.html").write_text("<!doctype html><p>metrics ui</p>", "utf-8")
    (ui_dir / "assets" / "app.js").write_text("console.log('ui');\n", "utf-8")
    (tmp_path / "secret.html").write_text("outside", "utf-8")
    store = MetricStore()
    store.publish(metric(sequence=3))

    with running_server(store, ui_dir=ui_dir) as (server, _):
        host, port = server.address

        status, headers, body = _get(host, port, "/")
        assert status == 200
        assert headers["content-type"] == "text/html; charset=utf-8"
        assert b"metrics ui" in body

        status, headers, body = _get(host, port, "/assets/app.js")
        assert status == 200
        assert headers["content-type"] == "text/javascript; charset=utf-8"
        assert body.startswith(b"console.log")

        # API routes keep precedence over static files.
        status, _, body = _get(host, port, "/api/v1/metrics/latest?symbol=ABM")
        assert status == 200
        assert json.loads(body)["metric"]["transport_sequence"] == 3

        for traversal in (
            "/../secret.html",
            "/assets/../../secret.html",
            "/%2e%2e/secret.html",
            "/..%2fsecret.html",
        ):
            status, _, body = _get(host, port, traversal)
            assert status == 404, traversal
            assert b"outside" not in body

        status, _, _ = _get(host, port, "/missing.html")
        assert status == 404


def test_root_stays_not_found_without_ui_dir() -> None:
    with running_server(MetricStore()) as (server, _):
        host, port = server.address
        status, _, body = _get(host, port, "/")
        assert status == 404
        assert json.loads(body) == {"error": "not_found"}


def test_rejects_missing_ui_dir(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="ui_dir"):
        MetricsBridgeServer(port=0, ui_dir=tmp_path / "nope")


def test_http_latest_and_sse_endpoints() -> None:
    store = MetricStore()
    published = store.publish(metric(sequence=11))

    with running_server(store) as (server, _):
        host, port = server.address
        connection = http.client.HTTPConnection(host, port, timeout=2)
        connection.request("GET", "/api/v1/metrics/latest?symbol=ABM")
        response = connection.getresponse()
        assert response.status == 200
        assert json.loads(response.read())["metric"]["transport_sequence"] == 11
        connection.close()

        stream = http.client.HTTPConnection(host, port, timeout=2)
        stream.request("GET", "/api/v1/metrics/stream?symbol=ABM")
        response = stream.getresponse()
        assert response.status == 200
        lines = [response.readline().decode("utf-8").rstrip("\n") for _ in range(6)]
        assert f"id: {published.event_id}" in lines
        assert "event: metric" in lines
        data_line = next(line for line in lines if line.startswith("data: "))
        assert json.loads(data_line[6:])["transport_sequence"] == 11
        stream.close()
