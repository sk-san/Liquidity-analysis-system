"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");

global.window = {
  MetricsUI: {
    format: {
      parseMetric: JSON.parse,
      nsToMs: (value) => Number(value / 1000000n),
    },
  },
};
global.MetricsUI = global.window.MetricsUI;

require("../assets/stream.js");

const { SymbolSeries, normalizeBookLevels } = window.MetricsUI.stream;

function record(sequence, overrides = {}) {
  return {
    transport_sequence: sequence,
    exchange_time_ns: 1_000_000_000 + sequence,
    synchronized: true,
    midprice: 100.5,
    microprice: 100.4,
    best_bid_price: 100,
    best_bid_quantity: 20,
    best_ask_price: 101,
    best_ask_quantity: 30,
    quoted_spread: 1,
    top_of_book_imbalance: -0.2,
    bid_liquidity_provision_ratio: 0.4,
    ask_liquidity_provision_ratio: 0.5,
    bid_visible_depth: 20,
    ask_visible_depth: 30,
    trade_count: 0,
    traded_volume: 0,
    last_trade_price: null,
    order_count: 2,
    bid_levels: [
      {
        price: 100,
        visible_quantity: 20,
        visible_mm_quantity: 8,
        visible_order_count: 2,
      },
    ],
    ask_levels: [
      {
        price: 101,
        visible_quantity: 30,
        visible_mm_quantity: 15,
        visible_order_count: 3,
      },
    ],
    ...overrides,
  };
}

test("normalizes compact L2 rows and rejects invalid MM depth", () => {
  assert.deepEqual(
    normalizeBookLevels([
      {
        price: 100,
        visible_quantity: 20,
        visible_mm_quantity: 8,
        visible_order_count: 2,
      },
    ]),
    [{ price: 100, qty: 20, mmQty: 8, orderCount: 2 }]
  );
  assert.equal(
    normalizeBookLevels([
      { price: 100, visible_quantity: 2, visible_mm_quantity: 3 },
    ]),
    null
  );
});

test("caps browser snapshots at the displayed top ten levels", () => {
  const levels = Array.from({ length: 12 }, (_, index) => ({
    price: 100 - index,
    visible_quantity: index + 1,
    visible_mm_quantity: 0,
  }));
  assert.equal(normalizeBookLevels(levels).length, 10);
});

test("retains only the latest book instead of copying it into chart history", () => {
  const series = new SymbolSeries("ABM");
  assert.equal(series.push(record(1)), true);
  assert.deepEqual(series.book.bids[0], {
    price: 100,
    qty: 20,
    mmQty: 8,
    orderCount: 2,
  });
  assert.equal("bids" in series.points[0], false);
  assert.equal("asks" in series.points[0], false);

  assert.equal(
    series.push(
      record(2, {
        bid_levels: [
          { price: 100, visible_quantity: 25, visible_mm_quantity: 10 },
        ],
      })
    ),
    true
  );
  assert.equal(series.book.bids[0].qty, 25);
  assert.equal(series.points.length, 2);
});

test("legacy records remain chartable and report no level snapshot", () => {
  const series = new SymbolSeries("ABM");
  const legacy = record(1);
  delete legacy.bid_levels;
  delete legacy.ask_levels;
  assert.equal(series.push(legacy), true);
  assert.equal(series.book, null);
  assert.equal(series.points.length, 1);
});

test("duplicate replay records do not replace the current book", () => {
  const series = new SymbolSeries("ABM");
  series.push(record(2));
  const current = series.book;
  assert.equal(series.push(record(2, { bid_levels: [] })), false);
  assert.equal(series.book, current);
});
