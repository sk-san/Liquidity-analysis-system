# Metrics UI

A dependency-free browser dashboard for the metric stream served by
`components/metrics-bridge`. Plain HTML, CSS, and JavaScript with canvas
rendering — no build step, no npm, nothing to install.

## Run it

`make run` serves the UI automatically at `http://127.0.0.1:8765/` while a
full-system run is active (`make run METRICS_HOLD=1` keeps it up after the
pipeline drains). To browse an archived run:

```bash
make run-metrics-bridge METRICS_INPUT=state/full-system/<run-id>/metrics.ndjson
```

The page can also be opened straight from disk; it then talks to
`http://127.0.0.1:8765`. A different bridge can be selected with a query
parameter: `index.html?bridge=http://host:port`.

## What it shows

- **Instrument bar** — midprice, quoted spread, microprice, best bid/ask with
  size, market-maker share of visible depth (the research signal), top-of-book
  imbalance, traded volume.
- **Strip charts on one shared time axis** — price with trade prints,
  market-maker share of visible depth, imbalance, and visible depth. Hovering
  or focusing the charts moves one crosshair across all channels and pins each
  channel's readout to that record; arrow keys step record by record.
- **Table view** — the most recent records as numbers, the accessible twin of
  the charts.
- **Event feed and banners** — connects, reconnects, replay-window resets,
  slow-client gaps, book desynchronization, and run changes are reported,
  never hidden.

Bid-side series are always blue, ask-side series always orange, in every
channel and both color schemes (the palette is colorblind-checked in light and
dark mode). Prices arrive as integer cents and are displayed in dollars.

## Contract details it honors

- **Nanosecond timestamps** exceed JavaScript's exact integer range, so
  `exchange_time_ns` is re-captured from the wire text as a `BigInt`; the
  plotted millisecond values are derived from it. ABIDES encodes its
  timezone-naive session clock (2021-02-05, 09:30–16:00) as UTC nanoseconds,
  so the clock is rendered in UTC and reads exactly as the scenario wall time.
- **Reconnect replays** are deduplicated by `transport_sequence`, so the
  browser's automatic `EventSource` recovery (with `Last-Event-ID`) never
  double-plots a record. A new `run_id` resets the page state.
- **`gap` and `reset` events** break the plotted lines instead of
  interpolating across lost records, and land in the event feed.
- **`synchronized=false`** records stay visible (they are diagnostics) but
  raise a persistent warning banner, per `docs/architecture.md`.
- Dense windows are decimated per pixel column keeping min and max, so spikes
  survive; the microprice is shown as a number but not plotted, because it
  differs from the midprice by at most half the quoted spread — sub-pixel at
  every zoom this page offers (the imbalance channel carries that signal).

## Files

```text
index.html        page structure
assets/styles.css theme tokens (light/dark), layout
assets/format.js  BigInt-safe parsing, price/time formatting
assets/stream.js  EventSource feed, series buffers, dedupe, event log
assets/charts.js  canvas strip-chart renderer (decimation, crosshair)
assets/app.js     page wiring: controls, tiles, table, render loop
```
