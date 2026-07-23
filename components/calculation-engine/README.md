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
