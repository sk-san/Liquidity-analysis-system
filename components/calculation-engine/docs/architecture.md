# Architecture

```text
ABIDES Exchange / Publisher
        |
        | canonical ordered events
        v
Pacing / rate-limit server
        |
        | RoutedMarketEvent
        v
CalculationEngine
  +-- per-symbol ShadowBook (authoritative read model)
  +-- MarketMetricsCalculator
  +-- latest immutable metric snapshot
```

## Responsibilities

### ABIDES

- Owns the authoritative matching engine and order book.
- Determines counterparties, execution prices and quantities.
- Emits snapshots and every state-changing incremental event.

### Pacing server

- Preserves event order and sequence numbers.
- Maps simulation time to wall-clock replay time.
- Does not coalesce order lifecycle events.

### Calculation engine

- Routes events by symbol.
- Applies events to a per-symbol L3 shadow book.
- Detects sequence gaps and exposes synchronization state.
- Computes read-only market metrics.
- Never performs live matching.

## Threading model

Each symbol/channel should have one ordered writer. `ShadowBook` is deliberately
lock-free and not thread-safe. If metrics are consumed by other threads, copy or
publish immutable `MarketMetrics` values after each successfully processed event.

## Recovery

When a sequence gap is detected, the book becomes desynchronized. Downstream
metrics are still exposed with `synchronized=false` for diagnostics but should not
be used for research measurements. Install a fresh `SnapshotEvent` before resuming.
