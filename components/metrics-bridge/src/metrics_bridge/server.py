from __future__ import annotations

import json
import logging
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.parse import parse_qs, urlsplit

from .model import METRIC_JSON_SCHEMA
from .store import GapNotice, MetricStore, PublishedMetric


logger = logging.getLogger(__name__)


class _MetricsHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        server_address: tuple[str, int],
        *,
        store: MetricStore,
        stop_event: threading.Event,
        cors_origins: tuple[str, ...],
        heartbeat_seconds: float,
    ) -> None:
        self.store = store
        self.stop_event = stop_event
        self.cors_origins = cors_origins
        self.heartbeat_seconds = heartbeat_seconds
        super().__init__(server_address, _MetricsRequestHandler)


class _MetricsRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "LiquidityMetricsBridge/0.1"

    @property
    def metrics_server(self) -> _MetricsHTTPServer:
        return self.server  # type: ignore[return-value]

    def log_message(self, format: str, *args: object) -> None:
        logger.info("%s - %s", self.address_string(), format % args)

    def _cors_origin(self) -> str | None:
        request_origin = self.headers.get("Origin")
        allowed = self.metrics_server.cors_origins
        if "*" in allowed:
            return "*"
        if request_origin is not None and request_origin in allowed:
            return request_origin
        return None

    def _origin_allowed(self) -> bool:
        request_origin = self.headers.get("Origin")
        return (
            request_origin is None
            or "*" in self.metrics_server.cors_origins
            or request_origin in self.metrics_server.cors_origins
        )

    def _common_headers(self) -> None:
        cors_origin = self._cors_origin()
        if cors_origin is not None:
            self.send_header("Access-Control-Allow-Origin", cors_origin)
            self.send_header("Vary", "Origin")

    def _send_json(self, status: HTTPStatus, value: object) -> None:
        body = json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self._common_headers()
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def _reject_disallowed_origin(self) -> bool:
        if self._origin_allowed():
            return False
        self._send_json(HTTPStatus.FORBIDDEN, {"error": "origin_not_allowed"})
        return True

    def do_OPTIONS(self) -> None:
        if self._reject_disallowed_origin():
            return
        self.send_response(HTTPStatus.NO_CONTENT)
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Last-Event-ID")
        self.send_header("Access-Control-Max-Age", "600")
        self._common_headers()
        self.end_headers()

    def do_GET(self) -> None:
        if self._reject_disallowed_origin():
            return
        request = urlsplit(self.path)
        if request.path == "/healthz":
            self._send_json(
                HTTPStatus.OK,
                {"status": "ok", **self.metrics_server.store.stats()},
            )
            return
        if request.path == "/api/v1/metrics/schema":
            self._send_json(HTTPStatus.OK, METRIC_JSON_SCHEMA)
            return
        if request.path == "/api/v1/metrics/latest":
            self._send_latest(parse_qs(request.query))
            return
        if request.path == "/api/v1/metrics/stream":
            self._send_stream(parse_qs(request.query))
            return
        self._send_json(HTTPStatus.NOT_FOUND, {"error": "not_found"})

    def _send_latest(self, query: dict[str, list[str]]) -> None:
        symbol = _one_query_value(query, "symbol")
        events = self.metrics_server.store.latest(symbol)
        if symbol is not None:
            if not events:
                self._send_json(
                    HTTPStatus.NOT_FOUND,
                    {"error": "symbol_not_found", "symbol": symbol},
                )
                return
            self._send_json(HTTPStatus.OK, {"metric": events[0].metric})
            return
        self._send_json(
            HTTPStatus.OK,
            {"metrics": [event.metric for event in events]},
        )

    def _send_stream(self, query: dict[str, list[str]]) -> None:
        symbol = _one_query_value(query, "symbol")
        replay_value = _one_query_value(query, "replay") or "latest"
        if replay_value not in ("latest", "all", "none"):
            self._send_json(
                HTTPStatus.BAD_REQUEST,
                {"error": "replay must be latest, all, or none"},
            )
            return
        last_event_id = self.headers.get("Last-Event-ID")
        subscription = self.metrics_server.store.subscribe(
            symbol=symbol,
            last_event_id=last_event_id,
            replay=replay_value,  # type: ignore[arg-type]
        )

        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache, no-transform")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self._common_headers()
        self.end_headers()

        try:
            self.wfile.write(b"retry: 1000\n\n")
            if subscription.reset is not None:
                self._write_sse("reset", subscription.reset)
            for event in subscription.backlog:
                self._write_metric(event)
            self.wfile.flush()

            while not self.metrics_server.stop_event.is_set():
                event = subscription.subscriber.get(
                    self.metrics_server.heartbeat_seconds
                )
                if event is None:
                    self.wfile.write(b": keepalive\n\n")
                elif isinstance(event, GapNotice):
                    self._write_sse(
                        "gap",
                        {
                            "reason": "slow_client",
                            "dropped_events": event.dropped_events,
                            "latest_dropped_event_id": event.latest_event_id,
                        },
                    )
                else:
                    self._write_metric(event)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        finally:
            self.close_connection = True
            self.metrics_server.store.unsubscribe(subscription.subscriber)

    def _write_metric(self, event: PublishedMetric) -> None:
        self.wfile.write(f"id: {event.event_id}\n".encode("ascii"))
        self.wfile.write(b"event: metric\n")
        self.wfile.write(b"data: ")
        self.wfile.write(event.json_data.encode("utf-8"))
        self.wfile.write(b"\n\n")

    def _write_sse(self, event_name: str, value: object) -> None:
        data = json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=False,
        )
        self.wfile.write(f"event: {event_name}\n".encode("ascii"))
        self.wfile.write(b"data: ")
        self.wfile.write(data.encode("utf-8"))
        self.wfile.write(b"\n\n")


def _one_query_value(query: dict[str, list[str]], name: str) -> str | None:
    values = query.get(name)
    if not values:
        return None
    return values[-1] or None


class MetricsBridgeServer:
    def __init__(
        self,
        *,
        host: str = "127.0.0.1",
        port: int = 8765,
        store: MetricStore | None = None,
        stop_event: threading.Event | None = None,
        cors_origins: tuple[str, ...] = ("*",),
        heartbeat_seconds: float = 15.0,
    ) -> None:
        if not 0 <= port <= 65_535:
            raise ValueError("port must be between 0 and 65535")
        if heartbeat_seconds <= 0:
            raise ValueError("heartbeat_seconds must be positive")
        if not cors_origins:
            raise ValueError("at least one CORS origin is required")
        self.store = store or MetricStore()
        self.stop_event = stop_event or threading.Event()
        self._httpd = _MetricsHTTPServer(
            (host, port),
            store=self.store,
            stop_event=self.stop_event,
            cors_origins=cors_origins,
            heartbeat_seconds=heartbeat_seconds,
        )

    @property
    def address(self) -> tuple[str, int]:
        host, port = self._httpd.server_address[:2]
        return str(host), int(port)

    def serve_forever(self) -> None:
        self._httpd.serve_forever(poll_interval=0.25)

    def shutdown(self) -> None:
        self.stop_event.set()
        self._httpd.shutdown()

    def server_close(self) -> None:
        self._httpd.server_close()
