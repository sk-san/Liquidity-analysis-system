# ABIDES Calculation Engine (C++17)

A calculation-engine repository built around the previously developed
ABIDES-JPMC-inspired L3 shadow order book.

The project reconstructs a read-only order book from canonical ABIDES market
events and computes market-quality metrics. It does **not** rerun matching.

## Repository layout

```text
abides_calculation_engine_cpp/
├── include/calculation_engine/
│   ├── market_data/       Routed event envelope and shared event types
│   ├── order_book/        Public shadow-book facade and snapshots
│   ├── indicators/        Spread, midprice, microprice, depth and imbalance
│   └── engine/            Multi-symbol calculation-engine orchestration
├── src/
│   ├── indicators/
│   └── engine/
├── libs/shadow_book/      Previous standalone shadow-book repository
├── apps/                  Runnable demonstration
├── tests/
│   ├── indicators/
│   └── integration/
└── docs/
```

## Processing model

```text
ABIDES authoritative book
        |
        | SNAPSHOT / ADD / CANCEL / MODIFY / REPLACE / EXECUTE
        v
Pacing server
        |
        v
CalculationEngine
        +-- TOPIX ShadowBook
        +-- BANKS ShadowBook
        +-- ...
                |
                v
        MarketMetrics snapshot
```

`CalculationEngine` requires an initial snapshot by default. It routes each event
to a per-symbol shadow book, applies the state transition, checks sequencing and
then computes metrics.

## Included metrics

- best bid and ask price/quantity;
- quoted spread;
- midprice;
- top-of-book microprice;
- top-of-book quantity imbalance;
- visible depth across a configurable number of price levels;
- trade count, traded volume and last trade price;
- deterministic visible-book checksum;
- synchronization status.

## Build

The stream service requires the ZeroMQ and zlib development libraries. On
macOS with Homebrew:

```bash
brew install zeromq zlib
```

On Debian/Ubuntu:

```bash
sudo apt-get install libzmq3-dev zlib1g-dev
```

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release
./build/release/calculation_engine_demo
```

Sanitizer build:

```bash
cmake --preset sanitized
cmake --build --preset sanitized -j
ctest --preset sanitized
```

## Stream service

`calculation_engine_service` is the transport adapter between the pacing
server and `CalculationEngine`. It connects a ZeroMQ `PULL` socket to the
pacing server's `PUSH` egress, validates and decodes canonical `MDP1`
MessagePack frames, applies market events in order, and emits synchronized
metric snapshots as JSON — newline-delimited on standard output by default,
or one record per ZeroMQ message when `--metrics-endpoint` names a `PUSH`
endpoint to connect (the metrics bridge binds the matching `PULL` side).
Metric sending never blocks the market-data path: records a stalled consumer
cannot absorb within the send timeout are dropped and counted on stderr.

Start it before or after the pacing server:

```bash
./build/release/calculation_engine_service \
  --endpoint tcp://127.0.0.1:5558 \
  --metrics-endpoint tcp://127.0.0.1:5560
```

Diagnostics and rejected/desynchronized events are written to standard error.
Lifecycle events are observed but are not passed to the calculation engine.
The service accepts zlib-compressed frames and resets all in-memory books when
the `run_id` changes.

Use `--help` for receive high-water mark, frame-size, metric-depth, and other
runtime options. The service intentionally does not perform matching or put
transport concerns inside the shadow book.

## Core API

```cpp
#include "calculation_engine/engine/calculation_engine.hpp"

namespace ce = calculation_engine;

ce::engine::CalculationEngine engine;

engine.apply({
    "TOPIX",
    wall_time_ns,
    ce::market_data::SnapshotEvent{
        snapshot_sequence,
        exchange_time_ns,
        "TOPIX",
        orders,
        std::nullopt
    }
});

auto result = engine.apply(routed_event);
if (result.metrics && result.metrics->synchronized) {
    // Publish or persist the immutable metric snapshot.
}
```

## Important boundaries

- ABIDES remains the write model and sole matching authority.
- The shadow book only applies already-decided order lifecycle events.
- Network transport and serialization are separate adapters.
- One ordered writer should apply events for each symbol/channel.
- After a sequence gap, do not use metrics until a snapshot restores sync.

See `docs/architecture.md` and `docs/integration_contract.md` for details.
