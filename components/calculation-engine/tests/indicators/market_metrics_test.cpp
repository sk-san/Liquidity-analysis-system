#include "calculation_engine/indicators/market_metrics.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ce = calculation_engine;

namespace {

constexpr ce::market_data::TimestampNs kHalfSecond = 500'000'000;
constexpr ce::market_data::TimestampNs kThreeQuarterSecond = 750'000'000;
constexpr ce::market_data::TimestampNs kOneSecond = 1'000'000'000;

[[noreturn]] void fail_check(
    const char* expression,
    const char* file,
    int line) {
    std::ostringstream message;
    message << file << ':' << line << ": check failed: " << expression;
    throw std::runtime_error(message.str());
}

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fail_check(#expression, __FILE__, __LINE__);                      \
        }                                                                     \
    } while (false)

ce::market_data::OrderImage order(
    ce::market_data::EntryId id,
    ce::market_data::Side side,
    ce::market_data::Price price,
    ce::market_data::Quantity quantity,
    bool is_market_maker = false,
    const std::string& symbol = "TEST",
    ce::market_data::Visibility visibility =
        ce::market_data::Visibility::Visible) {
    return ce::market_data::OrderImage{
        id,
        id,
        7,
        static_cast<ce::market_data::TimestampNs>(id),
        symbol,
        side,
        price,
        quantity,
        visibility,
        false,
        is_market_maker};
}

void must_apply(
    ce::order_book::ShadowBook& book,
    const ce::market_data::MarketEvent& event) {
    const auto result = book.apply(event);
    if (!result.ok()) {
        throw std::runtime_error("failed to prepare test book: " + result.message);
    }
}

bool near(double lhs, double rhs) {
    return std::fabs(lhs - rhs) < 1e-9;
}

void test_constructor_validates_depth() {
    bool threw = false;
    try {
        ce::indicators::MarketMetricsCalculator invalid(0);
        (void)invalid;
    } catch (const std::invalid_argument& error) {
        threw = true;
        CHECK(std::string(error.what()) == "depth_levels must be positive");
    }
    CHECK(threw);

    const ce::indicators::MarketMetricsCalculator default_depth;
    const ce::indicators::MarketMetricsCalculator custom_depth(3);
    CHECK(default_depth.depth_levels() == 10);
    CHECK(custom_depth.depth_levels() == 3);
}

void test_snapshot_metrics() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{10, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 10),
        order(2, ce::market_data::Side::Ask, 102, 30),
        order(3, ce::market_data::Side::Bid, 99, 5),
        order(4, ce::market_data::Side::Ask, 103, 7)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(2);
    const auto metrics = calculator.calculate(book, 123);

    CHECK(metrics.symbol == "TEST");
    CHECK(metrics.synchronized);
    CHECK(metrics.sequence == 10);
    CHECK(metrics.exchange_time_ns == 123);
    CHECK(metrics.order_count == 4);
    CHECK(metrics.best_bid_price == 100);
    CHECK(metrics.best_bid_quantity == 10);
    CHECK(metrics.best_ask_price == 102);
    CHECK(metrics.best_ask_quantity == 30);
    CHECK(metrics.quoted_spread == 2);
    CHECK(metrics.midprice.has_value());
    CHECK(metrics.microprice.has_value());
    CHECK(metrics.top_of_book_imbalance.has_value());
    CHECK(near(*metrics.midprice, 101.0));
    CHECK(near(*metrics.microprice, 100.5));
    CHECK(near(*metrics.top_of_book_imbalance, -0.5));
    CHECK(metrics.bid_visible_depth == 15);
    CHECK(metrics.ask_visible_depth == 37);
    CHECK(metrics.bid_levels.size() == 2);
    CHECK(metrics.bid_levels[0].price == 100);
    CHECK(metrics.bid_levels[0].visible_quantity == 10);
    CHECK(metrics.bid_levels[0].visible_mm_quantity == 0);
    CHECK(metrics.bid_levels[0].visible_order_count == 1);
    CHECK(metrics.bid_levels[1].price == 99);
    CHECK(metrics.bid_levels[1].visible_quantity == 5);
    CHECK(metrics.ask_levels.size() == 2);
    CHECK(metrics.ask_levels[0].price == 102);
    CHECK(metrics.ask_levels[0].visible_quantity == 30);
    CHECK(metrics.ask_levels[0].visible_order_count == 1);
    CHECK(metrics.ask_levels[1].price == 103);
    CHECK(metrics.ask_levels[1].visible_quantity == 7);
    CHECK(metrics.trade_count == 0);
    CHECK(metrics.traded_volume == 0);
    CHECK(!metrics.last_trade_price.has_value());
    CHECK(metrics.visible_checksum == book.visible_checksum());
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(!metrics.ask_liquidity_provision_ratio.has_value());
}

void test_empty_and_one_sided_books_leave_derived_prices_empty() {
    ce::order_book::ShadowBook empty("EMPTY");
    must_apply(
        empty,
        ce::market_data::SnapshotEvent{
            5, 0, "EMPTY", {}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator;
    const auto empty_metrics = calculator.calculate(empty, 42);
    CHECK(empty_metrics.synchronized);
    CHECK(empty_metrics.sequence == 5);
    CHECK(empty_metrics.order_count == 0);
    CHECK(!empty_metrics.best_bid_price.has_value());
    CHECK(!empty_metrics.best_bid_quantity.has_value());
    CHECK(!empty_metrics.best_ask_price.has_value());
    CHECK(!empty_metrics.best_ask_quantity.has_value());
    CHECK(!empty_metrics.quoted_spread.has_value());
    CHECK(!empty_metrics.midprice.has_value());
    CHECK(!empty_metrics.microprice.has_value());
    CHECK(!empty_metrics.top_of_book_imbalance.has_value());
    CHECK(empty_metrics.bid_visible_depth == 0);
    CHECK(empty_metrics.ask_visible_depth == 0);
    CHECK(empty_metrics.bid_levels.empty());
    CHECK(empty_metrics.ask_levels.empty());

    ce::order_book::ShadowBook one_sided("ONE");
    must_apply(one_sided, ce::market_data::SnapshotEvent{6, 0, "ONE", {
        order(1, ce::market_data::Side::Bid, 100, 9, false, "ONE"),
        order(
            2,
            ce::market_data::Side::Ask,
            99,
            50,
            false,
            "ONE",
            ce::market_data::Visibility::Hidden)}, std::nullopt});

    const auto one_sided_metrics = calculator.calculate(one_sided, 42);
    CHECK(one_sided_metrics.order_count == 2);
    CHECK(one_sided_metrics.best_bid_price == 100);
    CHECK(one_sided_metrics.best_bid_quantity == 9);
    CHECK(!one_sided_metrics.best_ask_price.has_value());
    CHECK(!one_sided_metrics.best_ask_quantity.has_value());
    CHECK(!one_sided_metrics.quoted_spread.has_value());
    CHECK(!one_sided_metrics.midprice.has_value());
    CHECK(!one_sided_metrics.microprice.has_value());
    CHECK(!one_sided_metrics.top_of_book_imbalance.has_value());
    CHECK(one_sided_metrics.bid_visible_depth == 9);
    CHECK(one_sided_metrics.ask_visible_depth == 0);
    CHECK(one_sided_metrics.bid_levels.size() == 1);
    CHECK(one_sided_metrics.bid_levels[0].price == 100);
    CHECK(one_sided_metrics.bid_levels[0].visible_quantity == 9);
    CHECK(one_sided_metrics.bid_levels[0].visible_order_count == 1);
    CHECK(one_sided_metrics.ask_levels.empty());
}

void test_depth_limit_and_hidden_orders() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{20, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true),
        order(2, ce::market_data::Side::Bid, 100, 6),
        order(3, ce::market_data::Side::Bid, 99, 20),
        order(4, ce::market_data::Side::Bid, 98, 30),
        order(5, ce::market_data::Side::Ask, 101, 5),
        order(6, ce::market_data::Side::Ask, 102, 7, true),
        order(7, ce::market_data::Side::Ask, 103, 11),
        order(
            8,
            ce::market_data::Side::Bid,
            105,
            50,
            true,
            "TEST",
            ce::market_data::Visibility::Hidden),
        order(
            9,
            ce::market_data::Side::Ask,
            90,
            50,
            true,
            "TEST",
            ce::market_data::Visibility::Hidden)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(2);
    const auto metrics = calculator.calculate(book, 0);
    CHECK(metrics.order_count == 9);
    CHECK(metrics.best_bid_price == 100);
    CHECK(metrics.best_bid_quantity == 10);
    CHECK(metrics.best_ask_price == 101);
    CHECK(metrics.best_ask_quantity == 5);
    CHECK(metrics.bid_visible_depth == 30);
    CHECK(metrics.ask_visible_depth == 12);
    CHECK(metrics.bid_levels.size() == 2);
    CHECK(metrics.bid_levels[0].price == 100);
    CHECK(metrics.bid_levels[0].visible_quantity == 10);
    CHECK(metrics.bid_levels[0].visible_mm_quantity == 4);
    CHECK(metrics.bid_levels[0].visible_order_count == 2);
    CHECK(metrics.bid_levels[1].price == 99);
    CHECK(metrics.bid_levels[1].visible_quantity == 20);
    CHECK(metrics.bid_levels[1].visible_mm_quantity == 0);
    CHECK(metrics.bid_levels[1].visible_order_count == 1);
    CHECK(metrics.ask_levels.size() == 2);
    CHECK(metrics.ask_levels[0].price == 101);
    CHECK(metrics.ask_levels[0].visible_quantity == 5);
    CHECK(metrics.ask_levels[0].visible_mm_quantity == 0);
    CHECK(metrics.ask_levels[0].visible_order_count == 1);
    CHECK(metrics.ask_levels[1].price == 102);
    CHECK(metrics.ask_levels[1].visible_quantity == 7);
    CHECK(metrics.ask_levels[1].visible_mm_quantity == 7);
    CHECK(metrics.ask_levels[1].visible_order_count == 1);
}

void test_large_top_quantities_do_not_overflow_derived_metrics() {
    const auto quantity = std::numeric_limits<ce::market_data::Quantity>::max();
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{30, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, quantity),
        order(2, ce::market_data::Side::Ask, 102, quantity)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    const auto metrics = calculator.calculate(book, 0);
    CHECK(metrics.microprice.has_value());
    CHECK(metrics.top_of_book_imbalance.has_value());
    CHECK(near(*metrics.microprice, 101.0));
    CHECK(near(*metrics.top_of_book_imbalance, 0.0));
}

void test_trade_statistics_and_last_trade_are_forwarded() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{40, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 10),
        order(2, ce::market_data::Side::Ask, 102, 10)}, 99});

    ce::indicators::MarketMetricsCalculator calculator(1);
    auto metrics = calculator.calculate(book, 0);
    CHECK(metrics.trade_count == 0);
    CHECK(metrics.traded_volume == 0);
    CHECK(metrics.last_trade_price == 99);

    must_apply(book, ce::market_data::ExecuteEvent{
        41, 100, 2, std::nullopt, 102, 3, 7,
        ce::market_data::Side::Bid});
    metrics = calculator.calculate(book, 100);
    CHECK(metrics.trade_count == 1);
    CHECK(metrics.traded_volume == 3);
    CHECK(metrics.last_trade_price == 102);
    CHECK(metrics.best_ask_quantity == 7);
    CHECK(metrics.visible_checksum == book.visible_checksum());

    must_apply(book, ce::market_data::ExecuteEvent{
        42, 200, 2, std::nullopt, 101, 7, 0,
        ce::market_data::Side::Bid});
    metrics = calculator.calculate(book, 200);
    CHECK(metrics.trade_count == 2);
    CHECK(metrics.traded_volume == 10);
    CHECK(metrics.last_trade_price == 101);
    CHECK(!metrics.best_ask_price.has_value());
    CHECK(!metrics.quoted_spread.has_value());
}

void test_time_weighted_liquidity_ratios() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{50, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true),
        order(2, ce::market_data::Side::Bid, 100, 6),
        order(3, ce::market_data::Side::Ask, 101, 5, true),
        order(4, ce::market_data::Side::Ask, 101, 15)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    auto metrics = calculator.calculate(book, 0);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(!metrics.ask_liquidity_provision_ratio.has_value());

    must_apply(
        book,
        ce::market_data::ModifyEvent{51, kHalfSecond, 1, 8});
    metrics = calculator.calculate(book, kHalfSecond);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());

    metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(metrics.ask_liquidity_provision_ratio.has_value());
    // Bid: (4 * 0.5s + 8 * 0.5s) / (10 * 0.5s + 14 * 0.5s).
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.5));
    CHECK(near(*metrics.ask_liquidity_provision_ratio, 0.25));

    // The most recently completed ratio remains available during the next
    // window instead of flickering back to null between boundaries.
    metrics = calculator.calculate(book, kOneSecond + 250'000'000);
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.5));
    CHECK(near(*metrics.ask_liquidity_provision_ratio, 0.25));
}

void test_liquidity_ratio_distinguishes_missing_zero_and_full_liquidity() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{55, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 7, true)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);

    // The bid side was entirely market-maker liquidity for the first window,
    // while the absent ask side has no meaningful ratio.
    must_apply(book, ce::market_data::AddEvent{
        56,
        kOneSecond,
        order(2, ce::market_data::Side::Ask, 101, 9)});
    auto metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 1.0));
    CHECK(!metrics.ask_liquidity_provision_ratio.has_value());

    // The non-MM ask added at the boundary contributes to the next window and
    // produces a defined zero rather than null.
    metrics = calculator.calculate(book, 2 * kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(metrics.ask_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 1.0));
    CHECK(near(*metrics.ask_liquidity_provision_ratio, 0.0));
}

void test_liquidity_update_at_boundary_applies_to_next_window() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{65, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true),
        order(2, ce::market_data::Side::Bid, 100, 6)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);

    must_apply(
        book,
        ce::market_data::ModifyEvent{66, kOneSecond, 1, 8});
    auto metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));

    metrics = calculator.calculate(book, 2 * kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 4.0 / 7.0));
}

void test_same_timestamp_updates_do_not_accrue_phantom_time() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{75, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);

    must_apply(book, ce::market_data::AddEvent{
        76,
        0,
        order(2, ce::market_data::Side::Bid, 100, 6)});
    auto metrics = calculator.calculate(book, 0);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());

    metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));
}

void test_liquidity_crosses_boundary_and_retains_completed_ratio() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{85, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 2, true),
        order(2, ce::market_data::Side::Bid, 100, 8)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);

    must_apply(book, ce::market_data::ModifyEvent{
        86, kThreeQuarterSecond, 1, 8});
    must_apply(book, ce::market_data::ModifyEvent{
        87, kThreeQuarterSecond, 2, 2});
    auto metrics = calculator.calculate(book, kThreeQuarterSecond);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());

    // First window: (2 * 0.75s + 8 * 0.25s) / (10 * 1.0s) = 0.35.
    metrics = calculator.calculate(book, kOneSecond + 250'000'000);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.35));

    // The partial second window does not replace the completed result. Once
    // that window closes, its constant 8 / 10 ratio becomes the latest value.
    metrics = calculator.calculate(book, 2 * kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.8));
}

void test_zero_depth_window_clears_previous_liquidity_ratio() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{95, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true),
        order(2, ce::market_data::Side::Bid, 100, 6)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);

    must_apply(
        book,
        ce::market_data::DeleteEvent{96, kHalfSecond, 1});
    must_apply(
        book,
        ce::market_data::DeleteEvent{97, kHalfSecond, 2});
    (void)calculator.calculate(book, kHalfSecond);

    auto metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));

    metrics = calculator.calculate(book, kOneSecond + kHalfSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));

    metrics = calculator.calculate(book, 2 * kOneSecond);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());
}

void test_liquidity_ratio_uses_only_configured_depth() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{60, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 10),
        order(2, ce::market_data::Side::Bid, 99, 90, true),
        order(3, ce::market_data::Side::Ask, 101, 4),
        order(4, ce::market_data::Side::Ask, 102, 6, true)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);
    const auto metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_visible_depth == 10);
    CHECK(metrics.ask_visible_depth == 4);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(metrics.ask_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.0));
    CHECK(near(*metrics.ask_liquidity_provision_ratio, 0.0));
}

void test_liquidity_windows_are_independent_per_symbol() {
    ce::order_book::ShadowBook first("FIRST");
    ce::order_book::ShadowBook second("SECOND");
    must_apply(first, ce::market_data::SnapshotEvent{1, 0, "FIRST", {
        order(1, ce::market_data::Side::Bid, 100, 1, true, "FIRST"),
        order(2, ce::market_data::Side::Bid, 100, 1, false, "FIRST")},
        std::nullopt});
    must_apply(second, ce::market_data::SnapshotEvent{1, kHalfSecond, "SECOND", {
        order(3, ce::market_data::Side::Bid, 100, 1, true, "SECOND"),
        order(4, ce::market_data::Side::Bid, 100, 3, false, "SECOND")},
        std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(first, 0);
    (void)calculator.calculate(second, kHalfSecond);

    const auto first_metrics = calculator.calculate(first, kOneSecond);
    const auto early_second_metrics = calculator.calculate(second, kOneSecond);
    CHECK(first_metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*first_metrics.bid_liquidity_provision_ratio, 0.5));
    CHECK(!early_second_metrics.bid_liquidity_provision_ratio.has_value());

    const auto second_metrics = calculator.calculate(
        second, kOneSecond + kHalfSecond);
    CHECK(second_metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*second_metrics.bid_liquidity_provision_ratio, 0.25));
    CHECK(!second_metrics.ask_liquidity_provision_ratio.has_value());
}

void test_desynchronization_restarts_liquidity_window() {
    ce::order_book::ShadowBook book("TEST");
    const auto snapshot = [](ce::market_data::Sequence sequence,
                             ce::market_data::TimestampNs timestamp) {
        return ce::market_data::SnapshotEvent{sequence, timestamp, "TEST", {
            order(1, ce::market_data::Side::Bid, 100, 4, true),
            order(2, ce::market_data::Side::Bid, 100, 6)}, std::nullopt};
    };
    must_apply(book, snapshot(70, 0));

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);
    auto metrics = calculator.calculate(book, kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));

    const auto gap = book.apply(ce::market_data::ModifyEvent{
        72, kOneSecond + 250'000'000, 1, 5});
    CHECK(gap.code == ce::order_book::ApplyCode::GapDetected);
    metrics = calculator.calculate(book, kOneSecond + 250'000'000);
    CHECK(!metrics.synchronized);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());

    must_apply(book, snapshot(72, kOneSecond + 250'000'000));
    metrics = calculator.calculate(book, kOneSecond + 250'000'000);
    CHECK(metrics.synchronized);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());

    metrics = calculator.calculate(book, 2 * kOneSecond + 250'000'000);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));
}

void test_backwards_timestamp_restarts_liquidity_window() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{80, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true),
        order(2, ce::market_data::Side::Bid, 100, 6)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, kOneSecond);
    auto metrics = calculator.calculate(book, 2 * kOneSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());

    metrics = calculator.calculate(book, kHalfSecond);
    CHECK(!metrics.bid_liquidity_provision_ratio.has_value());

    metrics = calculator.calculate(book, kOneSecond + kHalfSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));
}

void test_multi_window_jump_uses_latest_completed_window() {
    ce::order_book::ShadowBook book("TEST");
    must_apply(book, ce::market_data::SnapshotEvent{90, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 4, true),
        order(2, ce::market_data::Side::Bid, 100, 6),
        order(3, ce::market_data::Side::Ask, 101, 5, true),
        order(4, ce::market_data::Side::Ask, 101, 15)}, std::nullopt});

    ce::indicators::MarketMetricsCalculator calculator(1);
    (void)calculator.calculate(book, 0);
    const auto metrics = calculator.calculate(
        book, 3 * kOneSecond + kHalfSecond);
    CHECK(metrics.bid_liquidity_provision_ratio.has_value());
    CHECK(metrics.ask_liquidity_provision_ratio.has_value());
    CHECK(near(*metrics.bid_liquidity_provision_ratio, 0.4));
    CHECK(near(*metrics.ask_liquidity_provision_ratio, 0.25));
}

struct TestCase {
    const char* name;
    void (*run)();
};

}  // namespace

int main() {
    const TestCase tests[] = {
        {"constructor validates depth", test_constructor_validates_depth},
        {"snapshot metrics", test_snapshot_metrics},
        {"empty and one-sided books", test_empty_and_one_sided_books_leave_derived_prices_empty},
        {"depth limit and hidden orders", test_depth_limit_and_hidden_orders},
        {"large top quantities", test_large_top_quantities_do_not_overflow_derived_metrics},
        {"trade statistics", test_trade_statistics_and_last_trade_are_forwarded},
        {"time-weighted liquidity", test_time_weighted_liquidity_ratios},
        {"liquidity ratio boundaries", test_liquidity_ratio_distinguishes_missing_zero_and_full_liquidity},
        {"liquidity boundary update", test_liquidity_update_at_boundary_applies_to_next_window},
        {"same-timestamp liquidity update", test_same_timestamp_updates_do_not_accrue_phantom_time},
        {"liquidity boundary crossing", test_liquidity_crosses_boundary_and_retains_completed_ratio},
        {"zero-depth liquidity window", test_zero_depth_window_clears_previous_liquidity_ratio},
        {"liquidity depth", test_liquidity_ratio_uses_only_configured_depth},
        {"per-symbol liquidity", test_liquidity_windows_are_independent_per_symbol},
        {"desynchronization reset", test_desynchronization_restarts_liquidity_window},
        {"backwards timestamp reset", test_backwards_timestamp_restarts_liquidity_window},
        {"multi-window jump", test_multi_window_jump_uses_latest_completed_window},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " market metrics test(s) failed\n";
        return 1;
    }

    std::cout << "all market metrics tests passed\n";
    return 0;
}
