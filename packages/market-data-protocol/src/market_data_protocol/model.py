from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Mapping

SCHEMA_VERSION = 1
PROTOCOL_VERSION = 1

INCREMENTAL_EVENT_TYPES = frozenset(
    {
        "ADD",
        "DELETE",
        "PARTIAL_CANCEL",
        "MODIFY",
        "REPLACE",
        "EXECUTE",
    }
)
SNAPSHOT_EVENT_TYPE = "SNAPSHOT"
LIFECYCLE_EVENT_TYPES = frozenset(
    {
        "SIMULATION_START",
        "SESSION_OPEN",
        "HEARTBEAT",
        "SESSION_CLOSE",
        "SIMULATION_END",
        "ERROR",
    }
)
MARKET_EVENT_TYPES = INCREMENTAL_EVENT_TYPES | {SNAPSHOT_EVENT_TYPE}
ALL_EVENT_TYPES = MARKET_EVENT_TYPES | LIFECYCLE_EVENT_TYPES

SIDES = frozenset({"BID", "ASK"})
VISIBILITIES = frozenset({"VISIBLE", "HIDDEN"})


class ProtocolValidationError(ValueError):
    """Raised when a canonical market-data message violates the contract."""


def _require_int(value: Any, name: str, *, minimum: int | None = None) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ProtocolValidationError(f"{name} must be an integer")
    if minimum is not None and value < minimum:
        raise ProtocolValidationError(f"{name} must be >= {minimum}")
    return value


def _require_str(value: Any, name: str, *, nonempty: bool = True) -> str:
    if not isinstance(value, str):
        raise ProtocolValidationError(f"{name} must be a string")
    if nonempty and not value:
        raise ProtocolValidationError(f"{name} must not be empty")
    return value


def validate_order_image(order: Mapping[str, Any]) -> None:
    required = {
        "entry_id",
        "order_id",
        "agent_id",
        "priority_time_ns",
        "symbol",
        "side",
        "price",
        "quantity",
        "visibility",
        "insert_by_id",
    }
    missing = required.difference(order)
    if missing:
        raise ProtocolValidationError(f"order image missing fields: {sorted(missing)}")

    _require_int(order["entry_id"], "entry_id", minimum=0)
    _require_int(order["order_id"], "order_id", minimum=0)
    _require_int(order["agent_id"], "agent_id", minimum=0)
    _require_int(order["priority_time_ns"], "priority_time_ns")
    _require_str(order["symbol"], "symbol")
    side = _require_str(order["side"], "side")
    if side not in SIDES:
        raise ProtocolValidationError(f"side must be one of {sorted(SIDES)}")
    _require_int(order["price"], "price")
    _require_int(order["quantity"], "quantity", minimum=1)
    visibility = _require_str(order["visibility"], "visibility")
    if visibility not in VISIBILITIES:
        raise ProtocolValidationError(
            f"visibility must be one of {sorted(VISIBILITIES)}"
        )
    if not isinstance(order["insert_by_id"], bool):
        raise ProtocolValidationError("insert_by_id must be a bool")


def validate_event_payload(event_type: str, payload: Mapping[str, Any]) -> None:
    if not isinstance(payload, Mapping):
        raise ProtocolValidationError("payload must be a mapping")

    if event_type == "ADD":
        validate_order_image(payload.get("order", {}))
    elif event_type == "DELETE":
        _require_int(payload.get("entry_id"), "entry_id", minimum=0)
    elif event_type == "PARTIAL_CANCEL":
        _require_int(payload.get("entry_id"), "entry_id", minimum=0)
        _require_int(payload.get("cancelled_quantity"), "cancelled_quantity", minimum=1)
        remaining = payload.get("remaining_quantity")
        if remaining is not None:
            _require_int(remaining, "remaining_quantity", minimum=0)
    elif event_type == "MODIFY":
        _require_int(payload.get("entry_id"), "entry_id", minimum=0)
        _require_int(payload.get("new_quantity"), "new_quantity", minimum=1)
    elif event_type == "REPLACE":
        _require_int(payload.get("old_entry_id"), "old_entry_id", minimum=0)
        validate_order_image(payload.get("replacement", {}))
    elif event_type == "EXECUTE":
        _require_int(payload.get("passive_entry_id"), "passive_entry_id", minimum=0)
        aggressor_entry_id = payload.get("aggressor_entry_id")
        if aggressor_entry_id is not None:
            _require_int(aggressor_entry_id, "aggressor_entry_id", minimum=0)
        _require_int(payload.get("execution_price"), "execution_price")
        _require_int(payload.get("executed_quantity"), "executed_quantity", minimum=1)
        remaining = payload.get("passive_remaining_quantity")
        if remaining is not None:
            _require_int(remaining, "passive_remaining_quantity", minimum=0)
        side = _require_str(payload.get("aggressor_side"), "aggressor_side")
        if side not in SIDES:
            raise ProtocolValidationError(
                f"aggressor_side must be one of {sorted(SIDES)}"
            )
    elif event_type == SNAPSHOT_EVENT_TYPE:
        orders = payload.get("orders")
        if not isinstance(orders, list):
            raise ProtocolValidationError("snapshot orders must be a list")
        for order in orders:
            if not isinstance(order, Mapping):
                raise ProtocolValidationError("snapshot order must be a mapping")
            validate_order_image(order)
        last_trade = payload.get("last_trade_price")
        if last_trade is not None:
            _require_int(last_trade, "last_trade_price")
    elif event_type in LIFECYCLE_EVENT_TYPES:
        return
    else:
        raise ProtocolValidationError(f"unsupported event_type: {event_type}")


@dataclass(frozen=True, slots=True)
class MarketDataEnvelope:
    """Canonical record shared by ABIDES, the pacing server and consumers.

    ``transport_sequence`` is globally monotonic for the emitter and is used for
    persistence/ACK/retry. ``source_sequence`` is monotonic per market-data
    channel and is used by a shadow book to detect gaps. A snapshot carries the
    last source sequence represented by its image; it does not increment that
    sequence.
    """

    run_id: str
    transport_sequence: int
    channel_id: str
    source_sequence: int
    sim_time_ns: int
    generated_wall_time_ns: int
    event_type: str
    symbol: str
    payload: Mapping[str, Any]
    schema_version: int = SCHEMA_VERSION

    def validate(self) -> None:
        if self.schema_version != SCHEMA_VERSION:
            raise ProtocolValidationError(
                f"unsupported schema_version={self.schema_version}"
            )
        _require_str(self.run_id, "run_id")
        _require_int(self.transport_sequence, "transport_sequence", minimum=1)
        _require_str(self.channel_id, "channel_id")
        _require_int(self.source_sequence, "source_sequence", minimum=0)
        _require_int(self.sim_time_ns, "sim_time_ns")
        _require_int(self.generated_wall_time_ns, "generated_wall_time_ns", minimum=0)
        _require_str(self.event_type, "event_type")
        _require_str(self.symbol, "symbol", nonempty=False)
        if self.event_type not in ALL_EVENT_TYPES:
            raise ProtocolValidationError(f"unsupported event_type={self.event_type}")
        if self.event_type in MARKET_EVENT_TYPES and not self.symbol:
            raise ProtocolValidationError("market-data events require a symbol")
        validate_event_payload(self.event_type, self.payload)

    @property
    def is_incremental(self) -> bool:
        return self.event_type in INCREMENTAL_EVENT_TYPES

    @property
    def is_snapshot(self) -> bool:
        return self.event_type == SNAPSHOT_EVENT_TYPE

    @property
    def is_lifecycle(self) -> bool:
        return self.event_type in LIFECYCLE_EVENT_TYPES

    def to_dict(self) -> dict[str, Any]:
        self.validate()
        return {
            "schema_version": self.schema_version,
            "run_id": self.run_id,
            "transport_sequence": self.transport_sequence,
            "channel_id": self.channel_id,
            "source_sequence": self.source_sequence,
            "sim_time_ns": self.sim_time_ns,
            "generated_wall_time_ns": self.generated_wall_time_ns,
            "event_type": self.event_type,
            "symbol": self.symbol,
            "payload": dict(self.payload),
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "MarketDataEnvelope":
        envelope = cls(
            schema_version=value.get("schema_version", SCHEMA_VERSION),
            run_id=value["run_id"],
            transport_sequence=value["transport_sequence"],
            channel_id=value["channel_id"],
            source_sequence=value["source_sequence"],
            sim_time_ns=value["sim_time_ns"],
            generated_wall_time_ns=value["generated_wall_time_ns"],
            event_type=value["event_type"],
            symbol=value.get("symbol", ""),
            payload=value.get("payload", {}),
        )
        envelope.validate()
        return envelope


def market_data_message(envelope: MarketDataEnvelope) -> dict[str, Any]:
    return {
        "protocol_version": PROTOCOL_VERSION,
        "kind": "MARKET_DATA",
        "body": envelope.to_dict(),
    }


def control_message(kind: str, **body: Any) -> dict[str, Any]:
    _require_str(kind, "kind")
    return {
        "protocol_version": PROTOCOL_VERSION,
        "kind": kind,
        "body": body,
    }


def validate_wire_message(message: Mapping[str, Any]) -> None:
    if message.get("protocol_version") != PROTOCOL_VERSION:
        raise ProtocolValidationError(
            f"unsupported protocol_version={message.get('protocol_version')}"
        )
    kind = _require_str(message.get("kind"), "kind")
    body = message.get("body")
    if not isinstance(body, Mapping):
        raise ProtocolValidationError("wire message body must be a mapping")
    if kind == "MARKET_DATA":
        MarketDataEnvelope.from_dict(body)
