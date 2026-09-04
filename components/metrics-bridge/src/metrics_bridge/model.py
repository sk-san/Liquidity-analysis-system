from __future__ import annotations

import json
import math
from collections.abc import Mapping
from typing import Any


class MetricValidationError(ValueError):
    """Raised when a calculation-engine metric record violates the contract."""


_INTEGER_FIELDS = (
    "transport_sequence",
    "sequence",
    "exchange_time_ns",
    "received_wall_time_ns",
    "order_count",
    "bid_visible_depth",
    "ask_visible_depth",
    "trade_count",
    "traded_volume",
    "visible_checksum",
)

_NULLABLE_INTEGER_FIELDS = (
    "best_bid_price",
    "best_bid_quantity",
    "best_ask_price",
    "best_ask_quantity",
    "quoted_spread",
    "last_trade_price",
)

_NULLABLE_NUMBER_FIELDS = (
    "midprice",
    "microprice",
    "top_of_book_imbalance",
    "bid_liquidity_provision_ratio",
    "ask_liquidity_provision_ratio",
)

_ORDER_BOOK_FIELDS = ("bid_levels", "ask_levels")
_ORDER_BOOK_LEVEL_FIELDS = (
    "price",
    "visible_quantity",
    "visible_mm_quantity",
)
_OPTIONAL_ORDER_BOOK_LEVEL_FIELDS = ("visible_order_count",)

REQUIRED_METRIC_FIELDS = frozenset(
    {
        "run_id",
        "symbol",
        "synchronized",
        *_INTEGER_FIELDS,
        *_NULLABLE_INTEGER_FIELDS,
        *_NULLABLE_NUMBER_FIELDS,
    }
)


def _field_schema(field: str) -> dict[str, Any]:
    if field in _INTEGER_FIELDS:
        return {"type": "integer"}
    if field in _NULLABLE_INTEGER_FIELDS:
        return {"type": ["integer", "null"]}
    if field in _NULLABLE_NUMBER_FIELDS:
        return {"type": ["number", "null"]}
    if field == "synchronized":
        return {"type": "boolean"}
    return {"type": "string", "minLength": 1}


ORDER_BOOK_LEVEL_SCHEMA: dict[str, Any] = {
    "type": "object",
    "required": list(_ORDER_BOOK_LEVEL_FIELDS),
    "properties": {
        field: {"type": "integer", "minimum": 0}
        for field in (*_ORDER_BOOK_LEVEL_FIELDS, *_OPTIONAL_ORDER_BOOK_LEVEL_FIELDS)
    },
    "additionalProperties": True,
}

ORDER_BOOK_LEVELS_SCHEMA: dict[str, Any] = {
    "type": "array",
    "items": ORDER_BOOK_LEVEL_SCHEMA,
}


METRIC_JSON_SCHEMA: dict[str, Any] = {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "$id": "urn:liquidity-analysis:calculated-market-metric:v1",
    "title": "CalculatedMarketMetric",
    "type": "object",
    "required": sorted(REQUIRED_METRIC_FIELDS),
    "properties": {
        **{field: _field_schema(field) for field in sorted(REQUIRED_METRIC_FIELDS)},
        **{field: ORDER_BOOK_LEVELS_SCHEMA for field in _ORDER_BOOK_FIELDS},
    },
    "additionalProperties": True,
}


def _reject_json_constant(value: str) -> None:
    raise MetricValidationError(f"non-finite JSON number is not allowed: {value}")


def _is_integer(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_integer(metric: Mapping[str, Any], field: str) -> None:
    value = metric[field]
    if not _is_integer(value):
        raise MetricValidationError(f"{field} must be an integer")


def _require_nullable_integer(metric: Mapping[str, Any], field: str) -> None:
    value = metric[field]
    if value is not None and not _is_integer(value):
        raise MetricValidationError(f"{field} must be an integer or null")


def _require_nullable_number(metric: Mapping[str, Any], field: str) -> None:
    value = metric[field]
    if value is None:
        return
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise MetricValidationError(f"{field} must be a number or null")
    if not math.isfinite(value):
        raise MetricValidationError(f"{field} must be finite")


def _validate_order_book_levels(metric: Mapping[str, Any], field: str) -> None:
    levels = metric[field]
    if not isinstance(levels, list):
        raise MetricValidationError(f"{field} must be an array")

    for index, level in enumerate(levels):
        location = f"{field}[{index}]"
        if not isinstance(level, Mapping):
            raise MetricValidationError(f"{location} must be an object")

        missing = set(_ORDER_BOOK_LEVEL_FIELDS).difference(level)
        if missing:
            raise MetricValidationError(f"{location} missing fields: {sorted(missing)}")

        for level_field in _ORDER_BOOK_LEVEL_FIELDS:
            value = level[level_field]
            if not _is_integer(value):
                raise MetricValidationError(f"{location}.{level_field} must be an integer")
            if value < 0:
                raise MetricValidationError(f"{location}.{level_field} must not be negative")

        for level_field in _OPTIONAL_ORDER_BOOK_LEVEL_FIELDS:
            if level_field not in level:
                continue
            value = level[level_field]
            if not _is_integer(value):
                raise MetricValidationError(f"{location}.{level_field} must be an integer")
            if value < 0:
                raise MetricValidationError(f"{location}.{level_field} must not be negative")

        if level["visible_mm_quantity"] > level["visible_quantity"]:
            raise MetricValidationError(
                f"{location}.visible_mm_quantity must not exceed visible_quantity"
            )


def validate_metric(value: object) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise MetricValidationError("metric record must be a JSON object")

    missing = REQUIRED_METRIC_FIELDS.difference(value)
    if missing:
        raise MetricValidationError(f"metric record missing fields: {sorted(missing)}")

    for field in ("run_id", "symbol"):
        item = value[field]
        if not isinstance(item, str) or not item:
            raise MetricValidationError(f"{field} must be a non-empty string")

    if not isinstance(value["synchronized"], bool):
        raise MetricValidationError("synchronized must be a boolean")

    for field in _INTEGER_FIELDS:
        _require_integer(value, field)
    for field in _NULLABLE_INTEGER_FIELDS:
        _require_nullable_integer(value, field)
    for field in _NULLABLE_NUMBER_FIELDS:
        _require_nullable_number(value, field)
    for field in _ORDER_BOOK_FIELDS:
        if field in value:
            _validate_order_book_levels(value, field)

    if value["transport_sequence"] < 1:
        raise MetricValidationError("transport_sequence must be positive")
    for field in (
        "sequence",
        "received_wall_time_ns",
        "order_count",
        "bid_visible_depth",
        "ask_visible_depth",
        "trade_count",
        "traded_volume",
        "visible_checksum",
    ):
        if value[field] < 0:
            raise MetricValidationError(f"{field} must not be negative")

    return dict(value)


def parse_metric_line(line: str) -> dict[str, Any]:
    try:
        value = json.loads(line, parse_constant=_reject_json_constant)
    except json.JSONDecodeError as error:
        raise MetricValidationError(f"invalid JSON: {error.msg}") from error
    return validate_metric(value)
