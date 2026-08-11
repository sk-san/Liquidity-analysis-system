# Liquidity analysis system

Run the complete ABIDES → pacing server → calculation engine pipeline with:

```bash
make run
```

`make run-all` is an equivalent, more explicit target. It builds and tests the
calculation engine, starts the background services, runs the `rmsc04` ABIDES
scenario, waits for all market-data messages to drain, and then shuts the
services down. Each run writes metrics and component logs below
`state/full-system/<run-id>/`.

For a short smoke run or a different scenario:

```bash
make run ABIDES_END_TIME=09:30:01
make run ABIDES_SCENARIO=rmsc03 ABIDES_SEED=7
```

The simulation clock is replayed at `PACING_SPEED=1000000` by default. Endpoints,
speed, scenario, seed, end time, and output directory can all be overridden with
the variables defined at the top of the `Makefile`.

The runnable calculation-engine service lives in
`components/calculation-engine`. Build and start its ZeroMQ receiver with:

```bash
make run-calculation-engine
```

By default it connects to the pacing server at `tcp://127.0.0.1:5558` and
writes synchronized metric snapshots as newline-delimited JSON. Override the
endpoint with `CALCULATION_ENGINE_EGRESS`.
