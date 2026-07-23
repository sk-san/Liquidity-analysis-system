# Canonical event contract

Use one monotonic sequence per symbol/channel. Monetary prices are integral ticks or
integral minor-price units; timestamps are simulation/exchange nanoseconds.

## ADD

```json
{
  "type": "ADD",
  "sequence": 101,
  "exchange_time_ns": 34200000000001,
  "order": {
    "entry_id": 9001,
    "order_id": 9001,
    "agent_id": 42,
    "priority_time_ns": 34200000000001,
    "symbol": "TOPIX",
    "side": "BID",
    "price": 276450,
    "quantity": 10,
    "visibility": "VISIBLE",
    "insert_by_id": false
  }
}
```

## EXECUTE

```json
{
  "type": "EXECUTE",
  "sequence": 102,
  "exchange_time_ns": 34200000000005,
  "passive_entry_id": 9001,
  "aggressor_entry_id": 9100,
  "execution_price": 276450,
  "executed_quantity": 4,
  "passive_remaining_quantity": 6,
  "aggressor_side": "ASK"
}
```

`passive_remaining_quantity` is authoritative. The shadow book verifies that it is
consistent with the locally reconstructed pre-event quantity. A mismatch forces
snapshot recovery.

## MODIFY

```json
{
  "type": "MODIFY",
  "sequence": 103,
  "exchange_time_ns": 34200000000010,
  "entry_id": 9001,
  "new_quantity": 12
}
```

A reduction keeps FIFO priority. An increase moves the entry to the back of its
visible or hidden queue, matching ABIDES `PriceLevel.update_order_quantity()`.

## DELETE and PARTIAL_CANCEL

```json
{
  "type": "DELETE",
  "sequence": 104,
  "exchange_time_ns": 34200000000011,
  "entry_id": 9001
}
```

```json
{
  "type": "PARTIAL_CANCEL",
  "sequence": 104,
  "exchange_time_ns": 34200000000011,
  "entry_id": 9001,
  "cancelled_quantity": 2,
  "remaining_quantity": 4
}
```

## REPLACE

A replace is one canonical event: remove `old_entry_id` and append the replacement
according to its price, side, visibility, and priority rules. Do not also emit a
DELETE and ADD for the same state transition.

## SNAPSHOT

```json
{
  "type": "SNAPSHOT",
  "snapshot_sequence": 250000,
  "exchange_time_ns": 34200100000000,
  "symbol": "TOPIX",
  "last_trade_price": 276500,
  "orders": []
}
```

The snapshot must include every physical L3 entry needed by the calculation engine.
After installation, the next incremental sequence must be `snapshot_sequence + 1`.

## Physical entry identity

ABIDES price-to-comply processing can relate visible and hidden entries to one
logical order. Use a distinct `entry_id` for each physical queue entry while keeping
the shared ABIDES identifier in `order_id`.
