#include "calculation_engine/indicators/market_metrics.hpp"

#include <stdexcept>
#include <vector>

namespace calculation_engine::indicators {
namespace {

constexpr market_data::TimestampNs kLiquidityWindowNs = 1'000'000'000;

struct VisibleDepth {
    market_data::Quantity total{};
    market_data::Quantity market_maker{};
};

VisibleDepth summarize_visible_depth(
    const std::vector<order_book::LevelView>& levels) {
    VisibleDepth result;
    for (const auto& level : levels) {
        result.total += level.visible_quantity;
        result.market_maker += level.visible_mm_quantity;
    }
    return result;
}

std::optional<double> liquidity_ratio(
    long double market_maker_quantity_time,
    long double total_quantity_time,
    long double total_time) {
    if (total_quantity_time <= 0.0L) {
        return std::nullopt;
    }
    if (total_time <= 0.0L){
        return std::nullopt;
    }
    return static_cast<double>((market_maker_quantity_time / total_time)/ (total_quantity_time / total_time));
}

}  // namespace

MarketMetricsCalculator::MarketMetricsCalculator(std::size_t depth_levels)
    : depth_levels_(depth_levels) {
    if (depth_levels_ == 0) {
        throw std::invalid_argument("depth_levels must be positive");
    }
}

MarketMetricsCalculator::LiquidityRatios
MarketMetricsCalculator::update_liquidity_ratios(
    const std::string& symbol,
    bool book_synchronized,
    market_data::TimestampNs exchange_time_ns,
    market_data::Quantity bid_visible_quantity,
    market_data::Quantity bid_visible_mm_quantity,
    market_data::Quantity ask_visible_quantity,
    market_data::Quantity ask_visible_mm_quantity) {
    // Never carry an integral across a period in which the book was known to be
    // stale. The next synchronized snapshot starts a fresh measurement window.
    if (!book_synchronized) {
        liquidity_states_.erase(symbol);
        return {};
    }

    auto& state = liquidity_states_[symbol];
    const auto initialize = [&] {
        state = LiquidityWindowState{};
        state.initialized = true;
        state.window_start_ns = exchange_time_ns;
        state.last_update_ns = exchange_time_ns;
        state.bid_visible_quantity = bid_visible_quantity;
        state.bid_visible_mm_quantity = bid_visible_mm_quantity;
        state.ask_visible_quantity = ask_visible_quantity;
        state.ask_visible_mm_quantity = ask_visible_mm_quantity;
    };

    if (!state.initialized || exchange_time_ns < state.last_update_ns) {
        // A backwards timestamp usually means a replay/restart. Mixing the two
        // time domains would create a negative duration, so restart cleanly.
        initialize();
        return {};
    }

    const auto accumulate = [&](market_data::TimestampNs duration_ns) {
        const auto duration = static_cast<long double>(duration_ns);
        state.denominator += duration;
        state.bid_quantity_time +=
            static_cast<long double>(state.bid_visible_quantity) * duration;
        state.bid_mm_quantity_time +=
            static_cast<long double>(state.bid_visible_mm_quantity) * duration;
        state.ask_quantity_time +=
            static_cast<long double>(state.ask_visible_quantity) * duration;
        state.ask_mm_quantity_time +=
            static_cast<long double>(state.ask_visible_mm_quantity) * duration;
    };

    const auto close_window = [&] {
        state.last_bid_ratio = liquidity_ratio(
            state.bid_mm_quantity_time, state.bid_quantity_time, state.denominator);
        state.last_ask_ratio = liquidity_ratio(
            state.ask_mm_quantity_time, state.ask_quantity_time, state.denominator);
        state.bid_quantity_time = 0.0L;
        state.bid_mm_quantity_time = 0.0L;
        state.ask_quantity_time = 0.0L;
        state.ask_mm_quantity_time = 0.0L;
        state.denominator = 0;
    };

    auto remaining_ns = exchange_time_ns - state.last_update_ns;
    const auto elapsed_in_window_ns =
        state.last_update_ns - state.window_start_ns;
    const auto until_boundary_ns =
        kLiquidityWindowNs - elapsed_in_window_ns;

    if (remaining_ns >= until_boundary_ns) {
        accumulate(until_boundary_ns);
        state.last_update_ns += until_boundary_ns;
        remaining_ns -= until_boundary_ns;
        close_window();
        state.window_start_ns = state.last_update_ns;

        // With no intervening event, every complete skipped window contains the
        // same quantities. Advance over all of them in O(1), retaining the most
        // recently completed ratio.
        const auto complete_windows = remaining_ns / kLiquidityWindowNs;
        if (complete_windows > 0) {
            const auto window_duration =
                static_cast<long double>(remaining_ns);
            state.last_bid_ratio = liquidity_ratio(
                static_cast<long double>(state.bid_visible_mm_quantity) *
                    window_duration,
                static_cast<long double>(state.bid_visible_quantity) *
                    window_duration,
                window_duration);
            state.last_ask_ratio = liquidity_ratio(
                static_cast<long double>(state.ask_visible_mm_quantity) *
                    window_duration,
                static_cast<long double>(state.ask_visible_quantity) *
                    window_duration,
                window_duration);

            const auto skipped_ns = complete_windows * kLiquidityWindowNs;
            state.last_update_ns += skipped_ns;
            state.window_start_ns += skipped_ns;
            remaining_ns -= skipped_ns;
        }
    }

    if (remaining_ns > 0) {
        accumulate(remaining_ns);
        state.last_update_ns += remaining_ns;
    }

    // The state observed at this event timestamp applies to the interval after
    // the event. The previous state was integrated up to this timestamp above.
    state.bid_visible_quantity = bid_visible_quantity;
    state.bid_visible_mm_quantity = bid_visible_mm_quantity;
    state.ask_visible_quantity = ask_visible_quantity;
    state.ask_visible_mm_quantity = ask_visible_mm_quantity;

    return {state.last_bid_ratio, state.last_ask_ratio};
}

MarketMetrics MarketMetricsCalculator::calculate(
    const order_book::ShadowBook& book,
    market_data::TimestampNs exchange_time_ns) {
    MarketMetrics result;
    result.symbol = book.symbol();
    result.sequence = book.last_sequence();
    result.exchange_time_ns = exchange_time_ns;
    result.synchronized = book.synchronized();
    result.order_count = book.order_count();

    const auto top = book.top();
    if (top.bid.has_value()) {
        result.best_bid_price = top.bid->first;
        result.best_bid_quantity = top.bid->second;
    }
    if (top.ask.has_value()) {
        result.best_ask_price = top.ask->first;
        result.best_ask_quantity = top.ask->second;
    }

    if (top.bid.has_value() && top.ask.has_value()) {
        const auto bid_price = top.bid->first;
        const auto bid_quantity = top.bid->second;
        const auto ask_price = top.ask->first;
        const auto ask_quantity = top.ask->second;

        result.quoted_spread = ask_price - bid_price;
        result.midprice =
            (static_cast<double>(bid_price) + static_cast<double>(ask_price)) / 2.0;

        const auto total_top_quantity =
            static_cast<long double>(bid_quantity) +
            static_cast<long double>(ask_quantity);
        if (total_top_quantity > 0.0L) {
            result.microprice = static_cast<double>(
                (static_cast<long double>(ask_price) *
                     static_cast<long double>(bid_quantity) +
                 static_cast<long double>(bid_price) *
                     static_cast<long double>(ask_quantity)) /
                total_top_quantity);
            result.top_of_book_imbalance = static_cast<double>(
                (static_cast<long double>(bid_quantity) -
                 static_cast<long double>(ask_quantity)) /
                total_top_quantity);
        }
    }

    const auto bid_levels = book.l2(market_data::Side::Bid, depth_levels_);
    const auto ask_levels = book.l2(market_data::Side::Ask, depth_levels_);
    const auto bid_depth = summarize_visible_depth(bid_levels);
    const auto ask_depth = summarize_visible_depth(ask_levels);
    result.bid_visible_depth = bid_depth.total;
    result.ask_visible_depth = ask_depth.total;

    const auto ratios = update_liquidity_ratios(
        result.symbol,
        result.synchronized,
        exchange_time_ns,
        bid_depth.total,
        bid_depth.market_maker,
        ask_depth.total,
        ask_depth.market_maker
    );
    result.bid_liquidity_provision_ratio = ratios.bid;
    result.ask_liquidity_provision_ratio = ratios.ask;

    const auto& trades = book.trade_stats();
    result.trade_count = trades.trade_count;
    result.traded_volume = trades.total_volume;
    result.last_trade_price = trades.last_trade_price;
    result.visible_checksum = book.visible_checksum();
    return result;
}

}  // namespace calculation_engine::indicators
