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

- **The headline: market-maker liquidity provision.** The bid and ask
  liquidity-provision ratios lead the page — a hero tile with both values and
  fill tracks, and the top, tallest strip chart (tagged *primary research
  signal*) with a live end-marker on each line. Everything else is context
  for this pair.
- **Instrument bar** — midprice, quoted spread, microprice, best bid/ask with
  size, top-of-book imbalance, traded volume.
- **Real-time L2 order book** — the top 10 visible bid and ask levels arrive
  with each metric event and are rendered as one atomic snapshot. The mirrored
  ladder shows price, visible size, visible market-maker size, cumulative depth,
  and depth bars normalized across both sides. Its summary reports spread,
  midpoint, full-ladder imbalance, and visible depth.
- **Strip charts on one shared time axis** — MM liquidity provision ratio,
  price with trade prints, imbalance, and visible depth. Hovering or focusing
  the charts moves one crosshair across all channels and pins each channel's
  readout to that record; arrow keys step record by record.
- **Table view** — the most recent records as numbers, the accessible twin of
  the charts.
- **Event feed and banners** — connects, reconnects, replay-window resets,
  slow-client gaps, book desynchronization, and run changes are reported,
  never hidden.

Bid-side data is always blue and ask-side data is always orange in the ladder,
tiles, readouts, and charts. Status green, amber, and red are kept separate from
those market-side semantics. Prices arrive as integer cents and are displayed
in dollars.

## Themes

The theme selector is saved in browser-local storage and offers four choices:

- **System** follows the operating-system preference, using Quartz in light
  mode and Carbon in dark mode.
- **Quartz** is the fixed light analyst palette.
- **Carbon** is a neutral dark palette intended for long monitoring sessions.
- **Midnight** is a cooler dark operations palette with stronger chart
  separation.

All four choices preserve bid-blue and ask-orange semantics. Changing themes
also updates the canvas charts immediately; it does not change the data or the
selected symbol.

## Contract details it honors

- **Nanosecond timestamps** exceed JavaScript's exact integer range, so
  `exchange_time_ns` is re-captured from the wire text as a `BigInt`; the
  plotted millisecond values are derived from it. ABIDES encodes its
  timezone-naive session clock (2021-02-05, 09:30–16:00) as UTC nanoseconds,
  so the clock is rendered in UTC and reads exactly as the scenario wall time.
- **Reconnect replays** are deduplicated by `transport_sequence`, so the
  browser's automatic `EventSource` recovery (with `Last-Event-ID`) never
  double-plots a record. A new `run_id` resets the page state.
- **Atomic L2 snapshots** travel in the same metric SSE event as their summary
  metrics. `bid_levels` and `ask_levels` are accepted only as a complete pair,
  and the browser replaces the selected symbol's previous ladder rather than
  merging levels from different events or symbols. Only the current snapshot
  per symbol is retained, while chart history remains bounded separately.
- **Pause and symbol selection** freeze the displayed chart time and capture
  the current order-book snapshot for every known symbol. Data continues to
  buffer in the background, and switching symbols while paused shows that
  symbol's frozen state rather than jumping its ladder back to live. Resume
  catches the whole display up to the newest records.
- **`gap` and `reset` events** break the plotted lines instead of
  interpolating across lost records, and land in the event feed.
- **`synchronized=false`** records stay visible (they are diagnostics) but
  raise a persistent warning banner. The ladder is marked **Desynced** and its
  depth is explicitly treated as stale diagnostic data until a fresh book
  snapshot restores synchronization, per `docs/architecture.md`.
- **Legacy aggregate-only records** remain supported because the L2 arrays are
  optional in the bridge schema. For recordings that predate them, the ladder
  reports **Aggregate only** while best bid/ask and aggregate visible depth
  continue to appear in the instrument bar and history chart.
- Dense windows are decimated per pixel column keeping min and max, so spikes
  survive; the microprice is shown as a number but not plotted, because it
  differs from the midprice by at most half the quoted spread — sub-pixel at
  every zoom this page offers (the imbalance channel carries that signal).

## Files

```text
index.html        page structure and accessible order-book tables
assets/styles.css Quartz/Carbon/Midnight theme tokens, responsive layout
assets/format.js  BigInt-safe parsing, price/time formatting
assets/stream.js  EventSource feed, series buffers, atomic L2 snapshots, dedupe
assets/charts.js  canvas strip-chart renderer (decimation, crosshair)
assets/app.js     page wiring: controls, order book, tiles, table, render loop
```
