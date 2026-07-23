# ABIDES-JPMC-inspired C++ Shadow Book

A dependency-free C++17 L3 shadow order book for a calculation engine that consumes
canonical market events produced by an ABIDES-JPMC exchange adapter.

## Scope

The component is a **read model**, not a matching engine:

- ABIDES decides matching, execution price, execution quantity, and counterparties.
- The shadow book applies `ADD`, `DELETE`, `PARTIAL_CANCEL`, `MODIFY`, `REPLACE`,
  `EXECUTE`, and full `SNAPSHOT` events.
- It reconstructs visible/hidden FIFO queues, L1/L2/L3 views, trade statistics,
  visible imbalance, and a deterministic checksum.
- It detects sequence gaps and blocks incrementals until a fresh snapshot arrives.

## ABIDES behavior mirrored

- Bid prices are sorted descending; ask prices ascending.
- FIFO is maintained within a price and visibility queue.
- Visible orders are represented separately from hidden orders.
- Quantity reductions retain queue priority.
- Quantity increases move the order to the back of its queue.
- Executions reduce or remove the passive resting entry only; no matching is rerun.

`entry_id` is distinct from logical `order_id`.  This lets a publisher assign unique
identities to physical visible/hidden entries, including price-to-comply pairs that
may relate to the same logical ABIDES order.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/shadow_book_demo
```

## Integration contract

The ABIDES-side publisher should emit a monotonically increasing sequence per
symbol/channel.  At startup or after a gap, send a full `SnapshotEvent` whose
`snapshot_sequence` is the last included incremental sequence.  Then send every
state-changing event without coalescing.

For executions, include at least:

- `passive_entry_id`
- `execution_price`
- `executed_quantity`
- `passive_remaining_quantity`
- `aggressor_side`

Providing `passive_remaining_quantity` lets the shadow book verify the delta against
ABIDES' authoritative result.  A disagreement marks the book desynchronized.

## Main API

```cpp
abides::shadow::ShadowBook book("TOPIX");
auto result = book.apply(event);

auto top = book.top();
auto bids = book.l2(abides::shadow::Side::Bid, 10);
auto orders = book.l3(abides::shadow::Side::Ask, 10, false);
auto checksum = book.visible_checksum();
auto errors = book.validate();
```

## Concurrency

`ShadowBook` is intentionally single-writer and has no internal locking. Feed events
through one ordered consumer thread. Publish immutable metric snapshots to other
threads rather than applying events concurrently.

## Deliberate exclusions

- Matching/crossing logic
- Network transport and serialization
- Persistence/journaling
- Counterfactual matching
- Exchange-specific auction logic

Those concerns should be separate modules so calculation latency or failures cannot
change the ABIDES simulation outcome.
