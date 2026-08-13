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

from metrics_bridge.ingest import ingest_path, ingest_stream
from metrics_bridge.model import MetricValidationError, parse_metric_line
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
