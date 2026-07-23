from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any

from .emitter import MarketDataEmitter


def timestamp_to_ns(value: Any) -> int:
    if isinstance(value, bool):
        raise TypeError("timestamp must not be bool")
    if isinstance(value, int):
        return value
    if hasattr(value, "value"):
        return int(value.value)
    if hasattr(value, "to_datetime64"):
        return int(value.to_datetime64().astype("datetime64[ns]").astype("int64"))
    return int(value)


def normalize_side(value: Any) -> str:
    text = getattr(value, "name", str(value)).upper()
    if "BID" in text or "BUY" in text:
        return "BID"
    if "ASK" in text or "SELL" in text:
        return "ASK"
    raise ValueError(f"cannot map ABIDES side {value!r}")


def split_safe_entry_id(order_id: int, visibility: str) -> int:
    """Generate distinct physical IDs for visible/hidden halves of one order."""

    return (int(order_id) << 1) | (1 if visibility == "HIDDEN" else 0)


@dataclass(slots=True)
class AbidesOrderAdapter:
    entry_id_policy: str = "split_safe"  # split_safe or passthrough

    def order_image(
        self,
        order: Any,
        *,
        entry_id: int | None = None,
        visibility: str = "VISIBLE",
        priority_time_ns: int | None = None,
        quantity: int | None = None,
        price: int | None = None,
        insert_by_id: bool = False,
    ) -> dict[str, Any]:
        visibility = visibility.upper()
        order_id = int(getattr(order, "order_id"))
        if entry_id is None:
            if self.entry_id_policy == "split_safe":
                entry_id = split_safe_entry_id(order_id, visibility)
            elif self.entry_id_policy == "passthrough":
                entry_id = order_id
            else:
                raise ValueError(f"unsupported entry_id_policy={self.entry_id_policy}")

        order_quantity = int(quantity if quantity is not None else getattr(order, "quantity"))
        order_price = price
        if order_price is None:
            order_price = getattr(order, "limit_price", None)
        if order_price is None:
            raise ValueError("order image requires a limit price")

        placed_at = priority_time_ns
        if placed_at is None:
            placed_at = timestamp_to_ns(getattr(order, "time_placed", 0))

        return {
            "entry_id": int(entry_id),
            "order_id": order_id,
            "agent_id": int(getattr(order, "agent_id")),
            "priority_time_ns": int(placed_at),
            "symbol": str(getattr(order, "symbol")),
            "side": normalize_side(getattr(order, "side")),
            "price": int(order_price),
            "quantity": order_quantity,
            "visibility": visibility,
            "insert_by_id": bool(insert_by_id),
        }


class AbidesMarketDataBridge:
    """Thin conversion layer called after ABIDES book mutations are committed."""

    def __init__(
        self,
        emitter: MarketDataEmitter,
        *,
        adapter: AbidesOrderAdapter | None = None,
    ) -> None:
        self.emitter = emitter
        self.adapter = adapter or AbidesOrderAdapter()

    def order_added(
        self,
        *,
        current_time: Any,
        order: Any,
        entry_id: int | None = None,
        visibility: str = "VISIBLE",
        insert_by_id: bool = False,
    ) -> None:
        image = self.adapter.order_image(
            order,
            entry_id=entry_id,
            visibility=visibility,
            insert_by_id=insert_by_id,
        )
        self.emitter.emit_incremental(
            channel_id=image["symbol"],
            symbol=image["symbol"],
            sim_time_ns=timestamp_to_ns(current_time),
            event_type="ADD",
            payload={"order": image},
        )

    def order_deleted(
        self,
        *,
        current_time: Any,
        symbol: str,
        entry_id: int,
    ) -> None:
        self.emitter.emit_incremental(
            channel_id=symbol,
            symbol=symbol,
            sim_time_ns=timestamp_to_ns(current_time),
            event_type="DELETE",
            payload={"entry_id": int(entry_id)},
        )

    def order_partially_cancelled(
        self,
        *,
        current_time: Any,
        symbol: str,
        entry_id: int,
        cancelled_quantity: int,
        remaining_quantity: int | None,
    ) -> None:
        self.emitter.emit_incremental(
            channel_id=symbol,
            symbol=symbol,
            sim_time_ns=timestamp_to_ns(current_time),
            event_type="PARTIAL_CANCEL",
            payload={
                "entry_id": int(entry_id),
                "cancelled_quantity": int(cancelled_quantity),
                "remaining_quantity": (
                    None if remaining_quantity is None else int(remaining_quantity)
                ),
            },
        )

    def order_modified(
        self,
        *,
        current_time: Any,
        symbol: str,
        entry_id: int,
        new_quantity: int,
    ) -> None:
        self.emitter.emit_incremental(
            channel_id=symbol,
            symbol=symbol,
            sim_time_ns=timestamp_to_ns(current_time),
            event_type="MODIFY",
            payload={"entry_id": int(entry_id), "new_quantity": int(new_quantity)},
        )

    def order_replaced(
        self,
        *,
        current_time: Any,
        old_entry_id: int,
        replacement_order: Any,
        replacement_entry_id: int | None = None,
        visibility: str = "VISIBLE",
    ) -> None:
        replacement = self.adapter.order_image(
            replacement_order,
            entry_id=replacement_entry_id,
            visibility=visibility,
        )
        self.emitter.emit_incremental(
            channel_id=replacement["symbol"],
            symbol=replacement["symbol"],
            sim_time_ns=timestamp_to_ns(current_time),
            event_type="REPLACE",
            payload={
                "old_entry_id": int(old_entry_id),
                "replacement": replacement,
            },
        )

    def order_executed(
        self,
        *,
        current_time: Any,
        symbol: str,
        passive_entry_id: int,
        execution_price: int,
        executed_quantity: int,
        passive_remaining_quantity: int | None,
        aggressor_side: Any,
        aggressor_entry_id: int | None = None,
    ) -> None:
        self.emitter.emit_incremental(
            channel_id=symbol,
            symbol=symbol,
            sim_time_ns=timestamp_to_ns(current_time),
            event_type="EXECUTE",
            payload={
                "passive_entry_id": int(passive_entry_id),
                "aggressor_entry_id": (
                    None if aggressor_entry_id is None else int(aggressor_entry_id)
                ),
                "execution_price": int(execution_price),
                "executed_quantity": int(executed_quantity),
                "passive_remaining_quantity": (
                    None
                    if passive_remaining_quantity is None
                    else int(passive_remaining_quantity)
                ),
                "aggressor_side": normalize_side(aggressor_side),
            },
        )

    def snapshot(
        self,
        *,
        current_time: Any,
        symbol: str,
        orders: Iterable[Mapping[str, Any]],
        last_trade_price: int | None = None,
    ) -> None:
        self.emitter.emit_snapshot(
            channel_id=symbol,
            symbol=symbol,
            sim_time_ns=timestamp_to_ns(current_time),
            orders=list(orders),
            last_trade_price=last_trade_price,
        )


def extract_orders_from_abides_book(
    order_book: Any,
    *,
    adapter: AbidesOrderAdapter | None = None,
) -> list[dict[str, Any]]:
    """Best-effort L3 extractor for the common ABIDES-JPMC PriceLevel layout.

    The function deliberately uses attribute inspection so the extension does
    not import ABIDES internals. Callers should add a fixture against their fork
    because price-to-comply and custom PriceLevel classes can require explicit
    entry IDs.
    """

    adapter = adapter or AbidesOrderAdapter()
    images: list[dict[str, Any]] = []
    for levels in (getattr(order_book, "bids", []), getattr(order_book, "asks", [])):
        for level in levels:
            visible_orders = getattr(level, "visible_orders", None)
            hidden_orders = getattr(level, "hidden_orders", None)
            if visible_orders is None and hidden_orders is None:
                visible_orders = getattr(level, "orders", [])
                hidden_orders = []
            for order in visible_orders or []:
                images.append(adapter.order_image(order, visibility="VISIBLE"))
            for order in hidden_orders or []:
                images.append(adapter.order_image(order, visibility="HIDDEN"))
    return images
