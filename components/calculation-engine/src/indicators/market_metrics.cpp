#include "calculation_engine/indicators/market_metrics.hpp"

#include <stdexcept>

namespace calculation_engine::indicators {
namespace {

market_data::Quantity sum_visible(
    const std::vector<order_book::LevelView>& levels) {
    market_data::Quantity total = 0;
    for (const auto& level : levels) {
        total += level.visible_quantity;
    }
    return total;
}

}  // namespace

MarketMetricsCalculator::MarketMetricsCalculator(std::size_t depth_levels)
    : depth_levels_(depth_levels) {
    if (depth_levels_ == 0) {
        throw std::invalid_argument("depth_levels must be positive");
    }
}

MarketMetrics MarketMetricsCalculator::calculate(
    const order_book::ShadowBook& book,
    market_data::TimestampNs exchange_time_ns) const {
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

        const auto total_top_quantity = bid_quantity + ask_quantity;
        if (total_top_quantity > 0) {
            result.microprice =
                (static_cast<double>(ask_price) * static_cast<double>(bid_quantity) +
                 static_cast<double>(bid_price) * static_cast<double>(ask_quantity)) /
                static_cast<double>(total_top_quantity);
            result.top_of_book_imbalance =
                static_cast<double>(bid_quantity - ask_quantity) /
                static_cast<double>(total_top_quantity);
        }
    }

    result.bid_visible_depth = sum_visible(
        book.l2(market_data::Side::Bid, depth_levels_));
    result.ask_visible_depth = sum_visible(
        book.l2(market_data::Side::Ask, depth_levels_));

    const auto& trades = book.trade_stats();
    result.trade_count = trades.trade_count;
    result.traded_volume = trades.total_volume;
    result.last_trade_price = trades.last_trade_price;
    result.visible_checksum = book.visible_checksum();
    return result;
}

}  // namespace calculation_engine::indicators
