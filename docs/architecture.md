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
        | JSON metric records over ZeroMQ PUSH
        | (NDJSON on stdout without --metrics-endpoint)
        v
Metrics bridge
  +-- latest-metric HTTP endpoint
  +-- browser-native SSE stream
  +-- static metrics-UI hosting
        |
        | SSE metric events carrying atomic metric + L2 snapshots
        v
Metrics UI (browser)
  +-- live top-10 L2 order book, instrument tiles and strip charts
  +-- gap / reset / desync surfacing
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

- Ingests metric records from a ZeroMQ PULL endpoint it binds (the full-system
  default), from stdin, or from an NDJSON file it follows.
- Validates calculation-engine metric records against the `metrics.ndjson` shape.
- Retains the latest metric per symbol and a bounded reconnect history.
- Delivers metrics to browsers through HTTP and Server-Sent Events.
- Reports replay-window and slow-client gaps explicitly instead of hiding loss.
- Optionally serves the static browser UI; it never interprets metric values.

### Metrics UI

- Renders the stream in the browser: an atomic top-10 L2 order-book ladder,
  instrument tiles, shared-axis strip charts, and a table view, with no
  third-party dependencies.
- Replaces each symbol's ladder from the `bid_levels` and `ask_levels` carried
  together in one metric SSE event; it never combines levels across events or
  symbols. Missing arrays remain valid for legacy aggregate-only recordings.
- Deduplicates reconnect replays by `transport_sequence` and resets on a new
  `run_id`.
- Draws reported gaps as breaks in the data and keeps desynchronized records
  visibly flagged as diagnostics; the ladder is frozen with the rest of the
  display on Pause and marked stale while the shadow book is desynchronized.

## Threading model

Each symbol/channel should have one ordered writer. `ShadowBook` is deliberately
lock-free and not thread-safe. If metrics are consumed by other threads, copy or
publish immutable `MarketMetrics` values after each successfully processed event.

## Recovery

When a sequence gap is detected, the book becomes desynchronized. Downstream
metrics are still exposed with `synchronized=false` for diagnostics but should not
be used for research measurements. Install a fresh `SnapshotEvent` before resuming.
