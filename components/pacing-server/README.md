# Pacing server

The server owns three ZeroMQ endpoints:

- `ROUTER` ingress from the ABIDES emitter;
- `PUSH` egress to one calculation-engine stream consumer;
- `REP` control endpoint for pause/resume/speed/stats/snapshot requests.

Inbound frames are persisted to SQLite before a cumulative ACK is returned. A
separate scheduler maps `sim_time_ns` to monotonic wall time and forwards every
canonical order event without coalescing. On restart, undelivered journal rows
are replayed.

```bash
abides-pacing-server \
  --ingress tcp://127.0.0.1:5557 \
  --egress tcp://127.0.0.1:5558 \
  --control tcp://127.0.0.1:5559 \
  --speed 1.0
```

The repository's `calculation_engine_service` connects a ZeroMQ `PULL` socket
to the egress endpoint, validates the canonical MessagePack envelope, and
passes market events to the C++ calculation engine:

```bash
components/calculation-engine/build/release/calculation_engine_service \
  --endpoint tcp://127.0.0.1:5558
```
