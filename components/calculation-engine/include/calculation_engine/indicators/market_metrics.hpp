#pragma once

#include "calculation_engine/market_data/market_event.hpp"
#include "calculation_engine/order_book/shadow_book.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace calculation_engine::indicators {

struct BookLevel {
    market_data::Price price{};
    market_data::Quantity visible_quantity{};
    market_data::Quantity visible_mm_quantity{};
    std::size_t visible_order_count{};
};

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
    std::vector<BookLevel> bid_levels;
    std::vector<BookLevel> ask_levels;

    std::uint64_t trade_count{};
    market_data::Quantity traded_volume{};

    // MM-visible quantity-time divided by total visible quantity-time over the
    // most recently completed one-second exchange-time window. The calculation
    // uses the same configured depth as bid_visible_depth/ask_visible_depth.
    std::optional<double> bid_liquidity_provision_ratio;
    std::optional<double> ask_liquidity_provision_ratio;
    std::optional<market_data::Price> last_trade_price;
    std::uint64_t visible_checksum{};
};

class MarketMetricsCalculator {
public:
    explicit MarketMetricsCalculator(std::size_t depth_levels = 10);

    // This operation advances per-symbol liquidity windows and is intentionally
    // non-const even though it only reads the supplied ShadowBook.
    [[nodiscard]] MarketMetrics calculate(
        const order_book::ShadowBook& book,
        market_data::TimestampNs exchange_time_ns);

    [[nodiscard]] std::size_t depth_levels() const noexcept { return depth_levels_; }

private:
    struct LiquidityWindowState {
        bool initialized{};
        market_data::TimestampNs window_start_ns{};
        market_data::TimestampNs last_update_ns{};
        long double denominator{0};

        market_data::Quantity bid_visible_quantity{};
        market_data::Quantity bid_visible_mm_quantity{};
        market_data::Quantity ask_visible_quantity{};
        market_data::Quantity ask_visible_mm_quantity{};

        long double bid_quantity_time{};
        long double bid_mm_quantity_time{};
        long double ask_quantity_time{};
        long double ask_mm_quantity_time{};

        std::optional<double> last_bid_ratio;
        std::optional<double> last_ask_ratio;
    };

    struct LiquidityRatios {
        std::optional<double> bid;
        std::optional<double> ask;
    };

    [[nodiscard]] LiquidityRatios update_liquidity_ratios(
        const std::string& symbol,
        bool book_synchronized,
        market_data::TimestampNs exchange_time_ns,
        market_data::Quantity bid_visible_quantity,
        market_data::Quantity bid_visible_mm_quantity,
        market_data::Quantity ask_visible_quantity,
        market_data::Quantity ask_visible_mm_quantity);

    std::size_t depth_levels_;
    std::unordered_map<std::string, LiquidityWindowState> liquidity_states_;
};

}  // namespace calculation_engine::indicators
