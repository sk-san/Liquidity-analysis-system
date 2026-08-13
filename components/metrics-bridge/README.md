# Metrics bridge

The metrics bridge validates the calculation engine's newline-delimited JSON
output and makes it available to a browser over HTTP and Server-Sent Events
(SSE). It has no third-party runtime dependencies. Passing `--ui-dir <path>`
additionally serves a static browser UI (such as `components/metrics-ui`) at
`/`; without the flag the bridge stays API-only.

## Live input

Receive metric records over ZeroMQ — the bridge binds a PULL socket and the
calculation engine connects to it with `--metrics-endpoint` (this is what
`make run` wires up, and it needs the optional `pyzmq` dependency:
`pip install 'liquidity-metrics-bridge[zmq]'`):

```bash
liquidity-metrics-bridge \
  --input tcp://127.0.0.1:5560 \
  --archive state/metrics.ndjson \
  --host 127.0.0.1 \
  --port 8765

calculation_engine_service --endpoint tcp://127.0.0.1:5558 \
  --metrics-endpoint tcp://127.0.0.1:5560
```

Each ZeroMQ message carries one JSON metric record. Without pyzmq the bridge
still supports its pipe mode — the calculation engine's stdout piped to stdin:

```bash
calculation_engine_service --endpoint tcp://127.0.0.1:5558 \
  | liquidity-metrics-bridge \
      --input - \
      --archive state/metrics.ndjson \
      --host 127.0.0.1 \
      --port 8765
```

The bridge can also follow an engine output file that is still growing:

```bash
liquidity-metrics-bridge \
  --input state/full-system/<run-id>/metrics.ndjson \
  --follow
```

## Browser API

- `GET /` — the static UI, when `--ui-dir` is set (API routes keep precedence;
  file paths are resolved strictly inside the UI directory).
- `GET /healthz` — ingestion and connection counters.
- `GET /api/v1/metrics/schema` — JSON Schema for metric records.
- `GET /api/v1/metrics/latest` — latest record for every symbol.
- `GET /api/v1/metrics/latest?symbol=ABM` — latest record for one symbol.
- `GET /api/v1/metrics/stream` — live SSE stream.
- `GET /api/v1/metrics/stream?symbol=ABM` — symbol-filtered stream.
- `GET /api/v1/metrics/stream?replay=all` — replay retained history, then stream.

The default stream sends the latest record for each symbol before live updates.
Each `metric` event has an SSE `id`. Browser `EventSource` reconnects with that
ID and the bridge replays subsequent retained records. If the requested ID is
older than the configured history window, the bridge emits a `reset` event
before the available records. A client that cannot keep up receives a `gap`
event rather than silently losing metrics.

```javascript
const metrics = new EventSource(
  "http://127.0.0.1:8765/api/v1/metrics/stream?symbol=ABM"
);
metrics.addEventListener("metric", event => {
  const value = JSON.parse(event.data);
  console.log(value.transport_sequence, value.midprice);
});
metrics.addEventListener("reset", event => console.warn(event.data));
metrics.addEventListener("gap", event => console.warn(event.data));
```

Nanosecond timestamp values exceed JavaScript's exact integer range. Clients
that require bit-exact timestamps should parse the event with a big-integer JSON
parser or retain those fields as decimal strings before converting the payload.
