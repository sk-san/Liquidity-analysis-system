# Architecture

```text
ABIDES Exchange / Publisher
        |
        | canonical ordered events
        v
Pacing / rate-limit server
        |
        | ZeroMQ PUSH / MDP1 MessagePack
        v
Calculation-engine service
        |
        | validated RoutedMarketEvent
        v
CalculationEngine
  +-- per-symbol ShadowBook (authoritative read model)
  +-- MarketMetricsCalculator
  +-- latest immutable metric snapshot
        |
        | NDJSON metric records over stdout
        v
Metrics bridge
  +-- latest-metric HTTP endpoint
  +-- browser-native SSE stream
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

- Receives and decodes paced frames in a transport-adapter executable.
- Routes events by symbol.
- Applies events to a per-symbol L3 shadow book.
- Detects sequence gaps and exposes synchronization state.
- Computes read-only market metrics.
- Never performs live matching.

### Metrics bridge

- Validates calculation-engine metric records against the `metrics.ndjson` shape.
- Retains the latest metric per symbol and a bounded reconnect history.
- Delivers metrics to browsers through HTTP and Server-Sent Events.
- Reports replay-window and slow-client gaps explicitly instead of hiding loss.

## Threading model

Each symbol/channel should have one ordered writer. `ShadowBook` is deliberately
lock-free and not thread-safe. If metrics are consumed by other threads, copy or
publish immutable `MarketMetrics` values after each successfully processed event.

## Recovery

When a sequence gap is detected, the book becomes desynchronized. Downstream
metrics are still exposed with `synchronized=false` for diagnostics but should not
be used for research measurements. Install a fresh `SnapshotEvent` before resuming.
