#pragma once

#include "calculation_engine/market_data/market_event.hpp"
#include "calculation_engine/order_book/shadow_book.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace calculation_engine::indicators {

struct MarketMetrics {
    std::string symbol;
    market_data::Sequence sequence{};
    market_data::TimestampNs exchange_time_ns{};
    bool synchronized{};
    std::size_t order_count{};

    std::optional<market_data::Price> best_bid_price;
    std::optional<market_data::Quantity> best_bid_quantity;
    std::optional<market_data::Price> best_ask_price;
    std::optional<market_data::Quantity> best_ask_quantity;

    std::optional<market_data::Price> quoted_spread;
    std::optional<double> midprice;
    std::optional<double> microprice;
    std::optional<double> top_of_book_imbalance;

    market_data::Quantity bid_visible_depth{};
    market_data::Quantity ask_visible_depth{};

    std::uint64_t trade_count{};
    market_data::Quantity traded_volume{};
    std::optional<market_data::Price> last_trade_price;
    std::uint64_t visible_checksum{};
};

class MarketMetricsCalculator {
public:
    explicit MarketMetricsCalculator(std::size_t depth_levels = 10);

    [[nodiscard]] MarketMetrics calculate(
        const order_book::ShadowBook& book,
        market_data::TimestampNs exchange_time_ns) const;

    [[nodiscard]] std::size_t depth_levels() const noexcept { return depth_levels_; }

private:
    std::size_t depth_levels_;
};

}  // namespace calculation_engine::indicators
