# Liquidity analysis system

Run the complete ABIDES → pacing server → calculation engine → browser metrics
bridge pipeline with:

```bash
make run
```

`make run-all` is an equivalent, more explicit target. It builds and tests the
calculation engine, starts the background services, runs the `rmsc04` ABIDES
scenario, waits for all market-data messages to drain, and then shuts the
services down. Each run writes metrics and component logs below
`state/full-system/<run-id>/`.

While a run is active, the browser dashboard and the underlying endpoints are
available at:

```text
http://127.0.0.1:8765/                      (live metrics UI)
http://127.0.0.1:8765/api/v1/metrics/latest
http://127.0.0.1:8765/api/v1/metrics/stream
```

The UI in `components/metrics-ui` plots price, the market-maker liquidity
provision ratio, top-of-book imbalance, and visible depth as live strip
charts, and
surfaces sync loss, stream gaps, and reconnects instead of hiding them. The
stream uses browser-native Server-Sent Events. See
`components/metrics-bridge/README.md` for filtering, replay, CORS, and
standalone usage.

The run script stops all services once the pipeline drains. Keep the bridge and
UI up for browsing afterwards with:

```bash
make run METRICS_HOLD=1
```

Browse an archived run again later without ABIDES:

```bash
make run-metrics-bridge METRICS_INPUT=state/full-system/<run-id>/metrics.ndjson
```

For a short smoke run or a different scenario:

```bash
make run ABIDES_END_TIME=09:30:01
make run ABIDES_SCENARIO=rmsc03 ABIDES_SEED=7
```

The simulation clock is replayed at `PACING_SPEED=1000000` by default. Endpoints,
speed, scenario, seed, end time, and output directory can all be overridden with
the variables defined at the top of the `Makefile`. The bridge bind address and
port are controlled by `METRICS_BRIDGE_HOST` and `METRICS_BRIDGE_PORT`.

The runnable calculation-engine service lives in
`components/calculation-engine`. Build and start its ZeroMQ receiver with:

```bash
make run-calculation-engine
```

By default it connects to the pacing server at `tcp://127.0.0.1:5558` and
writes synchronized metric snapshots as newline-delimited JSON to stdout;
with `--metrics-endpoint` it publishes them over ZeroMQ PUSH instead, which
is how `make run` connects it to the metrics bridge (endpoint variable
`METRICS_INGRESS`, default `tcp://127.0.0.1:5560`). Override the pacing
endpoint with `CALCULATION_ENGINE_EGRESS`.
