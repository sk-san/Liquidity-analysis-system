/* Live metric feed: one EventSource against the bridge, per-symbol series
 * buffers, duplicate suppression across reconnect replays, and an event log
 * for everything the bridge reports instead of hiding (gaps, resets). */
"use strict";

window.MetricsUI = window.MetricsUI || {};

MetricsUI.stream = (() => {
  const { parseMetric, nsToMs } = MetricsUI.format;

  const SERIES_CAP = 60000;
  const TRIM_CHUNK = 5000;

  class SymbolSeries {
    constructor(symbol) {
      this.symbol = symbol;
      this.points = [];
      this.lastSeq = 0;
      this.pendingBreak = false;
    }

    push(record) {
      if (record.transport_sequence <= this.lastSeq) {
        return false; // duplicate from a reconnect replay
      }
      this.lastSeq = record.transport_sequence;
      const exactNs = record.exchange_time_ns_exact;
      this.points.push({
        t: exactNs !== undefined ? nsToMs(exactNs) : record.exchange_time_ns / 1e6,
        tNs: exactNs,
        seq: record.transport_sequence,
        mid: record.midprice,
        micro: record.microprice,
        bid: record.best_bid_price,
        bidQ: record.best_bid_quantity,
        ask: record.best_ask_price,
        askQ: record.best_ask_quantity,
        spread: record.quoted_spread,
        imb: record.top_of_book_imbalance,
        lprBid: record.bid_liquidity_provision_ratio,
        lprAsk: record.ask_liquidity_provision_ratio,
        depthBid: record.bid_visible_depth,
        depthAsk: record.ask_visible_depth,
        trades: record.trade_count,
        volume: record.traded_volume,
        lastPx: record.last_trade_price,
        orders: record.order_count,
        sync: record.synchronized,
        brk: this.pendingBreak,
      });
      this.pendingBreak = false;
      if (this.points.length > SERIES_CAP + TRIM_CHUNK) {
        this.points.splice(0, this.points.length - SERIES_CAP);
      }
      return true;
    }

    latest() {
      return this.points.length ? this.points[this.points.length - 1] : null;
    }
  }

  class MetricsFeed {
    constructor(baseUrl) {
      this.baseUrl = baseUrl;
      this.state = "connecting";
      this.runId = null;
      this.series = new Map();
      this.log = [];
      this.recordCount = 0;
      this.lastMessageWall = 0;
      this.source = null;
      this.handlers = { data: [], state: [], log: [], symbols: [] };
    }

    on(name, handler) {
      this.handlers[name].push(handler);
    }

    emit(name, detail) {
      for (const handler of this.handlers[name]) handler(detail);
    }

    addLog(kind, text) {
      this.log.unshift({ wall: Date.now(), kind, text });
      if (this.log.length > 100) this.log.length = 100;
      this.emit("log", this.log[0]);
    }

    setState(state, detailText) {
      if (this.state === state) return;
      this.state = state;
      this.emit("state", state);
      if (detailText) this.addLog(state === "live" ? "info" : "warn", detailText);
    }

    connect() {
      const url = `${this.baseUrl}/api/v1/metrics/stream?replay=all`;
      this.source = new EventSource(url);
      this.source.addEventListener("open", () => {
        this.setState("live", "connected to metrics stream");
      });
      this.source.addEventListener("error", () => {
        if (this.source.readyState === EventSource.CLOSED) {
          this.setState("closed", "stream closed — reload once the bridge is back");
        } else {
          this.setState("reconnecting", "connection lost — retrying");
        }
      });
      this.source.addEventListener("metric", (event) => {
        this.lastMessageWall = Date.now();
        let record;
        try {
          record = parseMetric(event.data);
        } catch {
          this.addLog("warn", "discarded an unparsable metric event");
          return;
        }
        this.accept(record);
      });
      this.source.addEventListener("reset", () => {
        this.markBreaks();
        this.addLog(
          "warn",
          "stream reset: reconnected past the retained history window"
        );
      });
      this.source.addEventListener("gap", (event) => {
        this.markBreaks();
        let dropped = "some";
        try {
          dropped = JSON.parse(event.data).dropped_events;
        } catch {
          /* keep the generic wording */
        }
        this.addLog("warn", `gap: ${dropped} events dropped (client too slow)`);
      });
    }

    markBreaks() {
      for (const series of this.series.values()) series.pendingBreak = true;
    }

    accept(record) {
      if (record.run_id !== this.runId) {
        const isFirst = this.runId === null;
        this.runId = record.run_id;
        if (!isFirst) {
          this.series.clear();
          this.recordCount = 0;
          this.addLog("info", `new run started: ${record.run_id}`);
          this.emit("symbols", this.symbolList());
        }
      }
      let series = this.series.get(record.symbol);
      const isNewSymbol = series === undefined;
      if (isNewSymbol) {
        series = new SymbolSeries(record.symbol);
        this.series.set(record.symbol, series);
      }
      const previous = series.latest();
      if (!series.push(record)) return;
      this.recordCount += 1;
      if (previous && previous.sync && !record.synchronized) {
        this.addLog(
          "critical",
          `${record.symbol} desynchronized at seq ${record.transport_sequence}`
        );
      } else if (previous && !previous.sync && record.synchronized) {
        this.addLog("info", `${record.symbol} back in sync`);
      }
      if (isNewSymbol) this.emit("symbols", this.symbolList());
      this.emit("data", record.symbol);
    }

    symbolList() {
      return [...this.series.keys()].sort();
    }

    get(symbol) {
      return this.series.get(symbol) || null;
    }
  }

  function resolveBaseUrl() {
    const override = new URLSearchParams(location.search).get("bridge");
    if (override && /^https?:\/\//.test(override)) {
      return override.replace(/\/+$/, "");
    }
    if (location.protocol === "http:" || location.protocol === "https:") {
      return location.origin;
    }
    // Opened from disk: talk to the default local bridge.
    return "http://127.0.0.1:8765";
  }

  return { MetricsFeed, resolveBaseUrl };
})();
