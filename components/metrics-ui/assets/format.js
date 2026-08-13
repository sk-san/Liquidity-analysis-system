/* Parsing and formatting helpers. No dependencies. */
"use strict";

window.MetricsUI = window.MetricsUI || {};

MetricsUI.format = (() => {
  // Nanosecond timestamps exceed Number.MAX_SAFE_INTEGER, so JSON.parse
  // rounds them (by up to ~256 ns). The exact values are recaptured from the
  // wire text as BigInt; charts and clocks use the derived millisecond value.
  const NS_FIELDS = /"(exchange_time_ns|received_wall_time_ns)"\s*:\s*(-?\d+)/g;

  function parseMetric(rawJson) {
    const record = JSON.parse(rawJson);
    NS_FIELDS.lastIndex = 0;
    for (const match of rawJson.matchAll(NS_FIELDS)) {
      record[`${match[1]}_exact`] = BigInt(match[2]);
    }
    return record;
  }

  function nsToMs(ns) {
    return Number(ns / 1000000n);
  }

  // ABIDES prices are integer cents; midprice/microprice are float cents.
  const usd2 = new Intl.NumberFormat("en-US", {
    style: "currency",
    currency: "USD",
  });
  const usd3 = new Intl.NumberFormat("en-US", {
    style: "currency",
    currency: "USD",
    minimumFractionDigits: 3,
    maximumFractionDigits: 3,
  });
  const int = new Intl.NumberFormat("en-US");
  const compact = new Intl.NumberFormat("en-US", {
    notation: "compact",
    maximumFractionDigits: 1,
  });

  function price(cents) {
    if (cents === null || cents === undefined) return "—";
    const dollars = cents / 100;
    return Number.isInteger(cents * 10) && !Number.isInteger(cents)
      ? usd3.format(dollars)
      : Number.isInteger(cents)
        ? usd2.format(dollars)
        : usd3.format(dollars);
  }

  function cents(value) {
    if (value === null || value === undefined) return "—";
    return `${int.format(value)}¢`;
  }

  function qty(value) {
    if (value === null || value === undefined) return "—";
    return int.format(value);
  }

  function qtyCompact(value) {
    if (value === null || value === undefined) return "—";
    return compact.format(value);
  }

  function pct(ratio) {
    if (ratio === null || ratio === undefined) return "—";
    return `${(ratio * 100).toFixed(1)}%`;
  }

  function signed(value, digits = 2) {
    if (value === null || value === undefined) return "—";
    const text = Math.abs(value).toFixed(digits);
    return value < 0 ? `−${text}` : `+${text}`;
  }

  // ABIDES timestamps are the scenario's timezone-naive session clock
  // (2021-02-05, 09:30–16:00) encoded as UTC nanoseconds, so the simulation
  // clock is rendered in UTC to read exactly as the scenario wall time.
  const SIM_TZ = "UTC";
  const clockFmt = new Intl.DateTimeFormat("en-US", {
    timeZone: SIM_TZ,
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    fractionalSecondDigits: 3,
  });
  const tickSecFmt = new Intl.DateTimeFormat("en-US", {
    timeZone: SIM_TZ,
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
  const dateFmt = new Intl.DateTimeFormat("en-US", {
    timeZone: SIM_TZ,
    year: "numeric",
    month: "short",
    day: "numeric",
  });

  function simClock(ms) {
    if (ms === null || ms === undefined) return "--:--:--.---";
    return clockFmt.format(ms);
  }

  function simTick(ms) {
    return tickSecFmt.format(ms);
  }

  function simDate(ms) {
    if (ms === null || ms === undefined) return "";
    return dateFmt.format(ms);
  }

  const wallFmt = new Intl.DateTimeFormat("en-US", {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });

  function wallClock(ms) {
    return wallFmt.format(ms);
  }

  return {
    parseMetric,
    nsToMs,
    price,
    cents,
    qty,
    qtyCompact,
    pct,
    signed,
    simClock,
    simTick,
    simDate,
    wallClock,
  };
})();
