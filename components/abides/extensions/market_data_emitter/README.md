# ABIDES market-data emitter

This extension converts committed ABIDES order-book mutations into the canonical
market-data stream. It writes each frame to a local SQLite outbox before a
publisher thread sends it over a ZeroMQ `DEALER` socket. The simulation thread
never performs network I/O.

## Integration rule

Call the bridge only **after** ABIDES has committed a state transition. Do not
emit an order-submission request before matching, because the calculation engine
must receive the authoritative result rather than a second matching problem.

```python
from pathlib import Path
from abides_market_data_emitter import (
    AbidesMarketDataBridge,
    EmitterConfig,
    MarketDataEmitter,
)

emitter = MarketDataEmitter(
    run_id="experiment-001",
    config=EmitterConfig(
        endpoint="tcp://127.0.0.1:5557",
        journal_path=Path("state/emitter.sqlite3"),
    ),
)
emitter.start()
bridge = AbidesMarketDataBridge(emitter)

# After an order was actually inserted into the book:
bridge.order_added(current_time=exchange.current_time, order=order)
```

At startup and after a detected gap, emit an L3 snapshot built from the
authoritative ABIDES book. Incremental event source sequences are maintained per
symbol; snapshots carry the current source watermark and do not advance it.
