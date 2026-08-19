# Liquidity Analysis System

**The research question: do the order-book metrics we use as stand-ins for
high-frequency market making actually capture it?**

This repository is the measurement instrument built to answer that question — a
simulated market where the answer is knowable, wired to a real-time pipeline
that computes the proxies and the ground truth side by side, from the same book,
at the same instant.

## Demo

An `rmsc04` run streaming live into the dashboard — simulation clock 09:33 to
09:38, paced onto wall time, every metric computed from the book as it moves:

https://github.com/user-attachments/assets/4af9f9dd-e6b5-4e5c-8dee-39408138c2b1

What the clip is showing, from the top:

- **The ground truth, as two numbers.** Bid and ask liquidity provision ratio —
  the share of visible quantity-time posted by market-maker agents. Opening
  frame: **bid 100.0%, ask 2.9%**. Market makers are the *entire* visible bid
  and almost none of the offer.
- **The proxies, at the same instant.** Midprice $999.655, spread 1¢,
  top-of-book imbalance **−0.74** — "ask-heavy". An analyst with real market
  data reads a book leaning hard to the offer. The ground truth says the lean is
  *because* the market makers are all on one side. The proxy sees the shape; it
  cannot see the author. That gap, quantified, is the research question.
- **The order book, with the label attached.** The L2 ladder carries an `MM`
  column per price level, next to size — `26 / 26` down the whole bid, `0` on
  the offer. This is what no real feed can print, and it is where the headline
  number comes from.
- **Both families on one time axis.** The liquidity provision ratio (tagged
  *primary research signal*), then price with trade prints, imbalance, and
  visible depth — same axis, same records, so the guesses and the thing being
  guessed at can be read against each other directly. Watch the two ratio lines
  converge and cross midway through the clip — bid 24.6% against ask 25.0% — as
  market-maker share of the two sides evens out.

Bid series are blue and ask series orange in every channel. Hovering the charts
moves one crosshair across all of them; arrow keys step record by record.

---

## Background — why the question is open

Public market data is anonymous. An exchange feed tells you that 500 shares
appeared at the bid; it never tells you *who* posted them. So when researchers,
exchanges, and regulators want to talk about how much of the visible liquidity
is being provided by high-frequency market makers, they cannot measure it
directly. They measure something else and treat it as a stand-in:

- quoted spread — tight spreads are read as active market making;
- top-of-book imbalance and microprice — read as short-horizon quoting pressure;
- visible depth — read as posted liquidity.

These are *proxy state metrics*. They are used precisely because the thing they
stand for is unobservable — which is also why they are so rarely checked against
it. The proxy and its target have almost never been put in the same dataset.

That matters, because the proxies are load-bearing. Liquidity-risk measures,
execution-cost models, market-quality reports, and market-structure policy are
all built on top of them. If a proxy tracks market-maker presence only loosely —
or tracks it well in calm markets and badly in stressed ones — everything above
it inherits the error silently.

## Why a simulator

An agent-based simulation removes the obstacle that makes the question hard: in
a simulation, every order's author is known.

This project runs [ABIDES](components/abides) (the JPMC agent-based market
simulator) with the `rmsc04` scenario — 1 exchange, **2 adaptive market makers**,
102 value agents, 12 momentum agents, and 1000 noise agents trading against each
other through a real matching engine. Market-maker agents stamp every order they
place with `is_market_maker=True`, and that label is carried all the way down the
pipeline into the reconstructed book.

So the same book yields both halves of the comparison:

| | Computed from | Available in real markets? |
|---|---|---|
| **Proxy metrics** — spread, midprice, microprice, top-of-book imbalance, visible depth, trade flow | The visible book only, exactly as a real analyst would | Yes |
| **Ground truth** — market-maker liquidity provision ratio | The same book, plus the per-order MM label | **No** — this is what the proxies are guessing at |

The **liquidity provision ratio** is the headline metric: the share of visible
*quantity-time* posted by market-maker agents, computed per side, across the top
10 price levels, over the most recently completed one-second window of exchange
time. Quantity-time rather than a point-in-time snapshot, because a market maker
that quotes 100 lots for a full second is providing more liquidity than one that
flashes 100 lots for a millisecond — and a snapshot cannot tell the two apart.

A note on scope: *HFT* here means the adaptive market-maker agents, since they
are the fast, continuously-requoting liquidity providers in this scenario. That
is an operational definition, not the whole of what "HFT" means in a real
market, and it is a limit on how far any result generalizes.

## What is built

An end-to-end, real-time pipeline. Each stage is a separate, independently
runnable component with its own tests and README.

```text
ABIDES simulation  ──►  Pacing server  ──►  Calculation engine  ──►  Metrics bridge  ──►  Browser UI
   (Python)              (Python)              (C++17)                  (Python)          (vanilla JS)

 matching engine,      replays sim time      reconstructs an L3      validates records,   live strip charts,
 MM-labelled orders    onto wall time,       shadow book, computes   fans them out over   proxies and ground
                       journals to SQLite    proxies + ground truth  HTTP + SSE           truth on one axis
```

| Component | Language | Role |
|---|---|---|
| [`components/abides`](components/abides) | Python | Vendored ABIDES fork. Owns matching; extended so orders carry an `is_market_maker` label. |
| [`components/pacing-server`](components/pacing-server) | Python | ZeroMQ ROUTER/PUSH/REP. Maps simulation time to wall-clock replay time, journals every frame to SQLite before ACK, replays undelivered rows on restart. Never coalesces order events. |
| [`components/calculation-engine`](components/calculation-engine) | C++17 | The core. Decodes MessagePack frames, reconstructs a per-symbol L3 shadow book, detects sequence gaps, computes all metrics. |
| [`components/metrics-bridge`](components/metrics-bridge) | Python | Validates each metric record against a JSON schema, retains latest-per-symbol plus bounded history, serves HTTP + Server-Sent Events. No third-party runtime dependencies. |
| [`components/metrics-ui`](components/metrics-ui) | HTML/CSS/JS | Dependency-free canvas dashboard. No build step, no npm. |

### Design decisions worth calling out

The pipeline is built for a measurement to be *trustworthy*, which drives most
of the non-obvious choices:

- **The shadow book never matches.** ABIDES is the sole matching authority; the
  C++ engine only applies already-decided lifecycle events to a read-only
  replica. A second matcher would silently produce a second, wrong market.
- **Desynchronization is fatal to a measurement, so it is never hidden.** On a
  sequence gap the book is flagged `synchronized=false`, and the liquidity
  window is *discarded* rather than integrated across a stale interval — a
  fresh snapshot starts a new window. Records still flow, marked as
  diagnostics.
- **Loss is reported, not smoothed.** Replay-window resets, slow-client gaps,
  reconnects, and run changes surface in the API and are drawn as breaks in the
  charts. A continuous-looking line over missing data is worse than a visible
  hole.
- **A deterministic visible-book checksum** travels with every record, so a
  reconstruction bug shows up as a mismatch instead of as a plausible number.

Tested with ctest (C++: shadow book, metrics, decoder, integration) and pytest
(Python: protocol, pacing journal and clock, bridge), both run on every push by
[CI](.github/workflows/ci.yml).

## How to run it

Requires CMake, a C++17 compiler, ZeroMQ (`libzmq3-dev`), and Python ≥ 3.11.

```bash
make install-dev
```

```bash
make run
```

`make run` builds and tests the engine, starts the services, runs the `rmsc04`
scenario, drains every market-data message, and shuts down. Each run writes
metrics and per-component logs to `state/full-system/<run-id>/`.

While a run is active:

```text
http://127.0.0.1:8765/                      live dashboard
http://127.0.0.1:8765/api/v1/metrics/latest
http://127.0.0.1:8765/api/v1/metrics/stream (SSE)
```

The dashboard is the one in the [demo](#demo) above: ground truth on top,
proxies beneath it on a shared time axis. One crosshair moves across all
channels; arrow keys step record by record.

Useful variants:

```bash
make run METRICS_HOLD=1                        # keep bridge + UI up after the run drains
make run ABIDES_END_TIME=09:30:01              # short smoke run
make run ABIDES_SCENARIO=rmsc03 ABIDES_SEED=7  # different scenario / seed
```

Browse an archived run later, without ABIDES:

```bash
make run-metrics-bridge METRICS_INPUT=state/full-system/<run-id>/metrics.ndjson
```

Simulation time is replayed at `PACING_SPEED=100000` by default; endpoints,
speed, scenario, seed, end time, and output directory are all overridable via
the variables at the top of the [`Makefile`](Makefile).

Run the engine on its own against a pacing server:

```bash
make run-calculation-engine
```

It connects to `tcp://127.0.0.1:5558` and writes metric snapshots as
newline-delimited JSON to stdout; `--metrics-endpoint` publishes them over
ZeroMQ PUSH instead, which is how `make run` feeds the bridge.

## Status

**Built and working:** the full pipeline end to end — labelled simulation,
paced feed, book reconstruction, both metric families computed on the same
events, live streaming, and the browser dashboard. A run produces a
`metrics.ndjson` archive containing the proxies and the ground truth aligned
record by record, which is the dataset the question needs.

**Not built yet:** the analysis stage. Nothing in this repository yet computes
the correlation between the proxies and the ground truth, tests it across
regimes and seeds, or reports a result. That is the next piece of work, and the
question in the title is still open here.

## Repository layout

```text
components/
  abides/               vendored ABIDES fork (MM labelling)
  pacing-server/        simulation-time → wall-time replay, journalled
  calculation-engine/   C++17 shadow book, metrics, ZeroMQ service
  metrics-bridge/       validation, retention, HTTP + SSE
  metrics-ui/           dependency-free browser dashboard
packages/
  market-data-protocol/ canonical event schema shared by Python components
scripts/                run_full_system.py, run_abides_sim.py
docs/                   architecture, integration contract, event contract
```

Deeper detail: [`docs/architecture.md`](docs/architecture.md),
[`docs/integration_contract.md`](docs/integration_contract.md), and
[`docs/shadow_book_event_contract.md`](docs/shadow_book_event_contract.md).
