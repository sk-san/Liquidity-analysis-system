/* Page wiring: feed -> tiles, strip charts, table, event feed. */
"use strict";

(() => {
  const fmt = MetricsUI.format;
  const { StripChart, lowerBound } = MetricsUI.charts;
  const { MetricsFeed, resolveBaseUrl } = MetricsUI.stream;

  const $ = (id) => document.getElementById(id);

  const WINDOWS = { "2m": 120000, "5m": 300000, "15m": 900000, all: null };

  const state = {
    symbol: null,
    window: "5m",
    paused: false,
    frozenT1: null,
    frozenBooks: null,
    crossIndex: null,
    tableOpen: false,
    themeChoice: "system",
    dirty: true,
    lastWarn: null,
    lastTableRender: 0,
    lastTileRender: 0,
    lastBookRender: 0,
  };

  /* ---------- theme ---------- */

  const TOKEN_MAP = {
    bid: "--series-bid",
    ask: "--series-ask",
    price: "--series-price",
    trade: "--series-trade",
  };

  let theme = null;
  const THEME_CHOICES = new Set(["system", "light", "dark", "midnight"]);

  function readTheme() {
    const style = getComputedStyle(document.documentElement);
    const token = (name) => style.getPropertyValue(name).trim();
    const colors = {};
    for (const [name, cssVar] of Object.entries(TOKEN_MAP)) {
      colors[name] = token(cssVar);
    }
    theme = {
      surface: token("--surface"),
      grid: token("--grid"),
      baseline: token("--baseline"),
      muted: token("--muted"),
      color: (name) => colors[name],
    };
  }

  function storedTheme() {
    try {
      const saved = localStorage.getItem("liquidity-ui-theme");
      return THEME_CHOICES.has(saved) ? saved : "system";
    } catch {
      return "system";
    }
  }

  function applyTheme(choice, persist = true) {
    const selected = THEME_CHOICES.has(choice) ? choice : "system";
    state.themeChoice = selected;
    document.documentElement.dataset.theme = selected;
    $("themeSel").value = selected;
    if (persist) {
      try {
        localStorage.setItem("liquidity-ui-theme", selected);
      } catch {
        /* Theme selection still works when storage is unavailable. */
      }
    }
    readTheme();
    state.dirty = true;
  }

  /* ---------- charts ---------- */

  const pricePlain = new Intl.NumberFormat("en-US", {
    minimumFractionDigits: 2,
    maximumFractionDigits: 2,
  });

  const charts = [
    // The headline channel: market-maker liquidity provision, bid and ask.
    new StripChart($("c-lpr"), {
      height: 232,
      yDomain: [0, 1],
      yTicks: [0, 0.25, 0.5, 0.75, 1],
      yFormat: (v) => `${Math.round(v * 100)}%`,
      series: [
        { type: "line", key: "lprBid", color: "bid", endDot: true },
        { type: "line", key: "lprAsk", color: "ask", endDot: true },
      ],
    }),
    new StripChart($("c-price"), {
      height: 200,
      yDomain: "auto",
      yFormat: (v) => pricePlain.format(v / 100),
      series: [
        { type: "dots", key: "lastPx", changedKey: "trades", color: "trade" },
        { type: "line", key: "mid", color: "price" },
      ],
    }),
    new StripChart($("c-imb"), {
      height: 138,
      yDomain: [-1.08, 1.08],
      yTicks: [-1, -0.5, 0, 0.5, 1],
      yFormat: (v) => (v === 0 ? "0" : fmt.signed(v, 1)),
      series: [
        { type: "diverging", key: "imb", posColor: "bid", negColor: "ask" },
      ],
    }),
    new StripChart($("c-depth"), {
      height: 168,
      drawXLabels: true,
      yDomain: "auto",
      includeZero: true,
      yFormat: (v) => fmt.qtyCompact(v),
      series: [
        { type: "line", key: "depthBid", color: "bid", fillToZero: true },
        { type: "line", key: "depthAsk", color: "ask", fillToZero: true },
      ],
    }),
  ];

  /* ---------- feed ---------- */

  const baseUrl = resolveBaseUrl();
  const feed = new MetricsFeed(baseUrl);

  feed.on("data", (symbol) => {
    if (state.symbol === null) selectSymbol(symbol);
    if (symbol === state.symbol || state.symbol === null) state.dirty = true;
    renderRunLine();
  });

  feed.on("symbols", (symbols) => {
    const select = $("symbolSel");
    const current = state.symbol;
    select.replaceChildren();
    for (const symbol of symbols) {
      const option = document.createElement("option");
      option.value = symbol;
      option.textContent = symbol;
      select.append(option);
    }
    if (current && symbols.includes(current)) {
      select.value = current;
    } else if (symbols.length) {
      selectSymbol(symbols[0]);
      select.value = symbols[0];
    }
  });

  feed.on("state", () => {
    renderStatus();
    renderBanner();
    state.dirty = true;
  });

  feed.on("log", (entry) => {
    if (entry.kind === "warn" || entry.kind === "critical") {
      state.lastWarn = entry;
    }
    renderFeed();
    renderBanner();
    state.dirty = true;
  });

  function selectSymbol(symbol) {
    state.symbol = symbol;
    state.crossIndex = null;
    state.dirty = true;
  }

  /* ---------- data access ---------- */

  function activeSeries() {
    return state.symbol ? feed.get(state.symbol) : null;
  }

  function activeBook() {
    if (state.paused && state.frozenBooks instanceof Map) {
      return state.frozenBooks.get(state.symbol) || null;
    }
    const series = activeSeries();
    return series ? series.book : null;
  }

  function captureBooks() {
    const books = new Map();
    for (const symbol of feed.symbolList()) {
      const series = feed.get(symbol);
      books.set(symbol, series ? series.book : null);
    }
    return books;
  }

  // Latest visible point index; when paused, the display is frozen at the
  // pause instant while data keeps accumulating behind it.
  function latestIndex(points) {
    if (!points.length) return -1;
    if (!state.paused || state.frozenT1 === null) return points.length - 1;
    return lowerBound(points, state.frozenT1 + 0.001) - 1;
  }

  /* ---------- renders ---------- */

  function renderStatus() {
    const pill = $("conn");
    pill.dataset.state = feed.state;
    const label = pill.querySelector("span:last-child");
    const idleFor = feed.lastMessageWall
      ? Math.round((Date.now() - feed.lastMessageWall) / 1000)
      : 0;
    if (feed.state === "live") {
      if (feed.recordCount === 0) label.textContent = "Live · waiting for data";
      else if (idleFor > 10) label.textContent = `Live · idle ${idleFor}s`;
      else label.textContent = "Live";
    } else if (feed.state === "connecting") {
      label.textContent = "Connecting…";
    } else if (feed.state === "reconnecting") {
      label.textContent = "Reconnecting…";
    } else {
      label.textContent = "Disconnected";
    }
  }

  function renderBanner() {
    const banner = $("banner");
    const series = activeSeries();
    const latest = series ? series.points[latestIndex(series.points)] : null;
    let kind = null;
    let html = null;

    if (feed.state === "closed") {
      kind = "critical";
      html = ["Bridge unreachable.", " Restart it, then reload this page."];
    } else if (latest && latest.sync === false) {
      kind = "critical";
      html = [
        "Book desynchronized.",
        " Metrics are diagnostic only until a snapshot restores sync.",
      ];
    } else if (feed.state === "reconnecting") {
      kind = "warn";
      html = ["Connection lost.", " Retrying — the stream resumes on its own."];
    } else if (state.paused) {
      kind = "info";
      html = ["Display paused.", " Data keeps arriving; resume to catch up."];
    } else if (state.lastWarn && Date.now() - state.lastWarn.wall < 12000) {
      kind = state.lastWarn.kind === "critical" ? "critical" : "warn";
      html = ["", state.lastWarn.text];
    }

    if (!kind) {
      banner.hidden = true;
      return;
    }
    banner.hidden = false;
    banner.dataset.kind = kind;
    banner.replaceChildren();
    if (html[0]) {
      const strong = document.createElement("b");
      strong.textContent = html[0];
      banner.append(strong);
    }
    banner.append(html[1]);
  }

  function renderRunLine() {
    const line = $("runLine");
    const series = activeSeries();
    if (!feed.runId || !series) {
      line.textContent = "no run yet — start one with `make run`";
      return;
    }
    const latest = series.points[latestIndex(series.points)];
    line.replaceChildren();
    const sync = document.createElement("span");
    if (latest) {
      sync.className = latest.sync ? "sync-ok" : "sync-bad";
      sync.textContent = latest.sync ? "sync" : "desync";
    }
    line.append(
      `run ${feed.runId} · ${fmt.simDate(latest ? latest.t : null)} · ` +
        `seq ${latest ? fmt.qty(latest.seq) : "—"} · ` +
        `${fmt.qty(feed.recordCount)} records · `
    );
    line.append(sync);
  }

  function signalFill(valueId, fillId, ratio) {
    $(valueId).textContent = fmt.pct(ratio);
    $(fillId).style.width =
      ratio === null || ratio === undefined
        ? "0%"
        : `${Math.min(Math.max(ratio, 0), 1) * 100}%`;
  }

  function renderBookEmpty(body, message) {
    body.replaceChildren();
    const row = document.createElement("tr");
    row.className = "book-empty";
    const cell = document.createElement("td");
    cell.colSpan = 4;
    cell.textContent = message;
    row.append(cell);
    body.append(row);
  }

  function bookCell(text, className = "") {
    const cell = document.createElement("td");
    cell.textContent = text;
    if (className) cell.className = className;
    return cell;
  }

  function renderBookSide(bodyId, levels, side, maxQuantity, unavailable) {
    const body = $(bodyId);
    if (unavailable) {
      renderBookEmpty(body, "Level detail unavailable for this recording");
      return;
    }
    if (!levels.length) {
      renderBookEmpty(body, `No visible ${side}s`);
      return;
    }

    body.replaceChildren();
    const fragment = document.createDocumentFragment();
    let cumulative = 0;
    for (const level of levels) {
      cumulative += level.qty;
      const row = document.createElement("tr");
      const width = maxQuantity > 0 ? (level.qty / maxQuantity) * 100 : 0;
      row.style.setProperty("--depth-fill", `${Math.min(width, 100).toFixed(2)}%`);

      const price = bookCell(fmt.price(level.price), "price");
      const quantity = bookCell(fmt.qty(level.qty), "quantity");
      if (level.orderCount !== null) {
        quantity.title = `${fmt.qty(level.orderCount)} visible ${
          level.orderCount === 1 ? "order" : "orders"
        }`;
      }
      const mm = bookCell(fmt.qty(level.mmQty), "mm-share");
      mm.title =
        level.qty > 0
          ? `${fmt.pct(level.mmQty / level.qty)} of this level is market-maker liquidity`
          : "No visible quantity";
      const cum = bookCell(fmt.qty(cumulative), "cumulative");

      if (side === "bid") row.append(cum, quantity, mm, price);
      else row.append(price, mm, quantity, cum);
      fragment.append(row);
    }
    body.append(fragment);
  }

  function renderBook(book, latest) {
    const panel = $("orderBook");
    const badge = $("bookStatus");
    const badgeLabel = badge.querySelector("span:last-child");
    const unavailable = book === null;
    const bids = book ? book.bids : [];
    const asks = book ? book.asks : [];
    const maxQuantity = Math.max(
      1,
      ...bids.map((level) => level.qty),
      ...asks.map((level) => level.qty)
    );

    const legacy = unavailable && latest !== null;
    renderBookSide("bidBookRows", bids, "bid", maxQuantity, legacy);
    renderBookSide("askBookRows", asks, "ask", maxQuantity, legacy);

    const bidDepth = book
      ? bids.reduce((sum, level) => sum + level.qty, 0)
      : latest
        ? latest.depthBid
        : null;
    const askDepth = book
      ? asks.reduce((sum, level) => sum + level.qty, 0)
      : latest
        ? latest.depthAsk
        : null;
    const totalDepth =
      bidDepth !== null && askDepth !== null ? bidDepth + askDepth : 0;
    const depthImbalance = totalDepth > 0 ? (bidDepth - askDepth) / totalDepth : null;

    $("bookSpread").textContent = latest ? fmt.cents(latest.spread) : "—";
    $("bookMid").textContent = latest ? fmt.price(latest.mid) : "—";
    $("bookImbalance").textContent = fmt.signed(depthImbalance);
    $("bookDepth").textContent =
      bidDepth === null || askDepth === null
        ? "—"
        : `${fmt.qtyCompact(bidDepth)} bid · ${fmt.qtyCompact(askDepth)} ask`;

    let status = "waiting";
    let label = "Waiting";
    let note = "Waiting for the first atomic depth snapshot.";
    if (book && !book.sync) {
      status = "desynced";
      label = "Desynced";
      note =
        "Stale diagnostic depth — do not use this ladder until a fresh snapshot restores synchronization.";
    } else if (book && state.paused) {
      status = "paused";
      label = "Paused";
      note = "Depth is frozen with the display; incoming snapshots continue buffering.";
    } else if (book && feed.state !== "live") {
      status = "stale";
      label = "Last known";
      note = "Showing the last complete depth snapshot while the stream reconnects.";
    } else if (book) {
      status = "live";
      label = "Live";
      note =
        "Each ladder update is atomic with its metric event; bars compare visible size across both sides.";
    } else if (latest) {
      status = "legacy";
      label = "Aggregate only";
      note =
        "This recording predates L2 snapshots; best prices and aggregate depth remain available above.";
    }

    panel.dataset.state = status;
    badge.dataset.state = status;
    badgeLabel.textContent = label;
    $("bookNote").textContent = note;
    const stampPoint = book || latest;
    $("bookStamp").textContent = stampPoint
      ? `seq ${fmt.qty(stampPoint.seq)} · ${fmt.simClock(stampPoint.t)} sim`
      : "no snapshot";
  }

  function renderTiles(latest) {
    $("t-mid").textContent = latest ? fmt.price(latest.mid) : "—";
    $("t-mid-sub").textContent = latest
      ? `spread ${fmt.cents(latest.spread)} · micro ${fmt.price(latest.micro)}`
      : "waiting for data";
    $("t-bid").textContent = latest ? fmt.price(latest.bid) : "—";
    $("t-bid-sub").textContent =
      latest && latest.bidQ !== null
        ? `${fmt.qty(latest.bidQ)} shares at best`
        : "—";
    $("t-ask").textContent = latest ? fmt.price(latest.ask) : "—";
    $("t-ask-sub").textContent =
      latest && latest.askQ !== null
        ? `${fmt.qty(latest.askQ)} shares at best`
        : "—";
    signalFill("t-lpr-bid", "f-bid", latest ? latest.lprBid : null);
    signalFill("t-lpr-ask", "f-ask", latest ? latest.lprAsk : null);
    $("t-imb").textContent = latest ? fmt.signed(latest.imb) : "—";
    const imb = latest ? latest.imb : null;
    $("t-imb-sub").textContent =
      imb === null || imb === undefined
        ? "—"
        : Math.abs(imb) < 0.05
          ? "balanced"
          : imb > 0
            ? "bid-heavy"
            : "ask-heavy";
    $("imbNeedle").style.left =
      imb === null || imb === undefined
        ? "50%"
        : `${((imb + 1) / 2) * 100}%`;
    $("t-act").textContent = latest ? fmt.qtyCompact(latest.volume) : "—";
    $("t-act-sub").textContent = latest
      ? `${fmt.qty(latest.trades)} trades · ${fmt.qty(latest.orders)} orders`
      : "—";
  }

  function setReadout(id, text) {
    $(id).textContent = text;
  }

  function renderReadouts(point) {
    setReadout("ro-mid", point ? fmt.price(point.mid) : "—");
    setReadout("ro-last", point ? fmt.price(point.lastPx) : "—");
    setReadout("ro-micro", point ? fmt.price(point.micro) : "—");
    setReadout("ro-lpr-bid", point ? fmt.pct(point.lprBid) : "—");
    setReadout("ro-lpr-ask", point ? fmt.pct(point.lprAsk) : "—");
    setReadout("ro-imb", point ? fmt.signed(point.imb) : "—");
    setReadout("ro-depth-bid", point ? fmt.qty(point.depthBid) : "—");
    setReadout("ro-depth-ask", point ? fmt.qty(point.depthAsk) : "—");
  }

  function renderClock(latest) {
    $("clock").firstChild.textContent = fmt.simClock(latest ? latest.t : null);
  }

  function renderTable(points, iLatest) {
    const body = $("tableBody");
    body.replaceChildren();
    const first = Math.max(0, iLatest - 39);
    for (let i = iLatest; i >= first; i -= 1) {
      const p = points[i];
      const row = document.createElement("tr");
      if (!p.sync) row.className = "desynced";
      const cells = [
        fmt.simClock(p.t),
        fmt.price(p.mid),
        fmt.cents(p.spread),
        fmt.price(p.bid),
        fmt.qty(p.bidQ),
        fmt.price(p.ask),
        fmt.qty(p.askQ),
        fmt.signed(p.imb),
        fmt.pct(p.lprBid),
        fmt.pct(p.lprAsk),
        fmt.qty(p.depthBid),
        fmt.qty(p.depthAsk),
        fmt.qty(p.trades),
        fmt.qty(p.volume),
        p.sync ? "ok" : "lost",
      ];
      for (const value of cells) {
        const cell = document.createElement("td");
        cell.textContent = value;
        row.append(cell);
      }
      body.append(row);
    }
  }

  function renderAll(now) {
    const series = activeSeries();
    const points = series ? series.points : [];
    const iLatest = latestIndex(points);
    const latest = iLatest >= 0 ? points[iLatest] : null;
    const book = activeBook();

    const windowMs = WINDOWS[state.window];
    const t1 = latest ? latest.t : 0;
    const t0 = latest
      ? windowMs === null
        ? points[0].t
        : t1 - windowMs
      : 0;
    const i0 = latest ? lowerBound(points, t0) : 0;
    const i1 = iLatest;

    if (state.crossIndex !== null && (state.crossIndex < i0 || state.crossIndex > i1)) {
      state.crossIndex = null;
    }

    const frame = {
      points,
      i0,
      i1,
      t0,
      t1,
      theme,
      crossIndex: state.crossIndex,
      message:
        feed.state === "closed"
          ? "bridge unreachable"
          : points.length
            ? "no data in this window"
            : "waiting for metrics…",
    };
    for (const chart of charts) chart.render(frame);

    positionTimeChip(frame);

    const inspecting = state.crossIndex !== null;
    renderReadouts(inspecting ? points[state.crossIndex] : latest);

    if (now - state.lastBookRender > 100) {
      state.lastBookRender = now;
      renderBook(book, latest);
    } else {
      state.dirty = true;
    }

    // Throttled targets get a trailing render: when a frame lands inside the
    // throttle window, stay dirty so the final state is never skipped.
    if (inspecting || now - state.lastTileRender > 250) {
      state.lastTileRender = now;
      renderTiles(latest);
      renderClock(latest);
      renderRunLine();
    } else {
      state.dirty = true;
    }
    if (state.tableOpen) {
      if (now - state.lastTableRender > 500) {
        state.lastTableRender = now;
        if (iLatest >= 0) renderTable(points, iLatest);
      } else {
        state.dirty = true;
      }
    }
  }

  function positionTimeChip(frame) {
    const chip = $("timeChip");
    if (frame.crossIndex === null || !frame.points.length) {
      chip.hidden = true;
      return;
    }
    const point = frame.points[frame.crossIndex];
    const canvas = $("c-depth");
    const strip = $("strip");
    const plotW = canvas.clientWidth - 58 - 12;
    const x =
      canvas.getBoundingClientRect().left -
      strip.getBoundingClientRect().left +
      58 +
      ((point.t - frame.t0) / Math.max(frame.t1 - frame.t0, 1)) * plotW;
    chip.hidden = false;
    chip.style.left = `${x}px`;
    chip.textContent = `${fmt.simClock(point.t)} sim`;
  }

  function renderFeed() {
    const list = $("feed");
    list.replaceChildren();
    if (!feed.log.length) {
      const item = document.createElement("li");
      item.className = "empty";
      item.textContent = "nothing yet";
      list.append(item);
      return;
    }
    for (const entry of feed.log.slice(0, 30)) {
      const item = document.createElement("li");
      item.dataset.kind = entry.kind;
      const time = document.createElement("time");
      time.textContent = fmt.wallClock(entry.wall);
      const message = document.createElement("span");
      message.className = "msg";
      message.textContent = entry.text;
      item.append(time, message);
      list.append(item);
    }
  }

  /* ---------- crosshair ---------- */

  function pointerToIndex(clientX) {
    const series = activeSeries();
    if (!series || !series.points.length) return null;
    const points = series.points;
    const iLatest = latestIndex(points);
    if (iLatest < 0) return null;
    const canvas = $("c-price");
    const rect = canvas.getBoundingClientRect();
    const plotW = rect.width - 58 - 12;
    if (plotW <= 0) return null;
    const windowMs = WINDOWS[state.window];
    const t1 = points[iLatest].t;
    const t0 = windowMs === null ? points[0].t : t1 - windowMs;
    const t = t0 + ((clientX - rect.left - 58) / plotW) * (t1 - t0);
    const i0 = lowerBound(points, t0);
    let index = lowerBound(points, t);
    if (index > iLatest) index = iLatest;
    if (
      index > i0 &&
      Math.abs(points[index - 1].t - t) < Math.abs(points[index].t - t)
    ) {
      index -= 1;
    }
    return Math.min(Math.max(index, i0), iLatest);
  }

  const strip = $("strip");

  strip.addEventListener("pointermove", (event) => {
    const index = pointerToIndex(event.clientX);
    if (index !== state.crossIndex) {
      state.crossIndex = index;
      state.dirty = true;
    }
  });

  strip.addEventListener("pointerleave", () => {
    if (state.crossIndex !== null) {
      state.crossIndex = null;
      state.dirty = true;
    }
  });

  strip.addEventListener("keydown", (event) => {
    const series = activeSeries();
    if (!series || !series.points.length) return;
    const iLatest = latestIndex(series.points);
    const step = event.shiftKey ? 10 : 1;
    let handled = true;
    if (event.key === "Escape") {
      state.crossIndex = null;
    } else if (event.key === "ArrowLeft" || event.key === "Left") {
      state.crossIndex =
        state.crossIndex === null
          ? iLatest
          : Math.max(0, state.crossIndex - step);
    } else if (event.key === "ArrowRight" || event.key === "Right") {
      state.crossIndex =
        state.crossIndex === null
          ? iLatest
          : Math.min(iLatest, state.crossIndex + step);
    } else if (event.key === "Home") {
      state.crossIndex = 0;
    } else if (event.key === "End") {
      state.crossIndex = iLatest;
    } else {
      handled = false;
    }
    if (handled) {
      event.preventDefault();
      state.dirty = true;
    }
  });

  /* ---------- controls ---------- */

  $("symbolSel").addEventListener("change", (event) => {
    selectSymbol(event.target.value);
  });

  $("themeSel").addEventListener("change", (event) => {
    applyTheme(event.target.value);
  });

  for (const button of document.querySelectorAll("[data-win]")) {
    button.addEventListener("click", () => {
      state.window = button.dataset.win;
      for (const other of document.querySelectorAll("[data-win]")) {
        other.setAttribute(
          "aria-pressed",
          other === button ? "true" : "false"
        );
      }
      state.crossIndex = null;
      state.dirty = true;
    });
  }

  $("pauseBtn").addEventListener("click", () => {
    state.paused = !state.paused;
    const series = activeSeries();
    const latest = series ? series.latest() : null;
    state.frozenT1 = state.paused && latest ? latest.t : null;
    state.frozenBooks = state.paused ? captureBooks() : null;
    $("pauseBtn").setAttribute("aria-pressed", state.paused ? "true" : "false");
    $("pauseBtn").textContent = state.paused ? "Resume" : "Pause";
    state.dirty = true;
    renderBanner();
  });

  $("tableBtn").addEventListener("click", () => {
    state.tableOpen = !state.tableOpen;
    $("tableBtn").setAttribute(
      "aria-pressed",
      state.tableOpen ? "true" : "false"
    );
    $("tablePanel").hidden = !state.tableOpen;
    state.lastTableRender = 0;
    state.dirty = true;
  });

  /* ---------- boot ---------- */

  function layoutAll() {
    for (const chart of charts) chart.layout();
    state.dirty = true;
  }

  new ResizeObserver(layoutAll).observe(strip);

  window
    .matchMedia("(prefers-color-scheme: dark)")
    .addEventListener("change", () => {
      if (state.themeChoice === "system") {
        readTheme();
        state.dirty = true;
      }
    });

  setInterval(() => {
    renderStatus();
    renderBanner();
  }, 1000);

  function frameLoop(now) {
    if (state.dirty) {
      state.dirty = false;
      renderAll(now);
    }
    requestAnimationFrame(frameLoop);
  }

  applyTheme(storedTheme(), false);
  layoutAll();
  renderStatus();
  renderFeed();
  renderRunLine();
  feed.connect();
  requestAnimationFrame(frameLoop);
})();
