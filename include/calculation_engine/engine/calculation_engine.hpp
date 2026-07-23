#pragma once

#include "calculation_engine/indicators/market_metrics.hpp"
#include "calculation_engine/market_data/market_event.hpp"
#include "calculation_engine/order_book/shadow_book.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace calculation_engine::engine {

struct EngineConfig {
    bool strict_sequence{true};
    bool require_initial_snapshot{true};
    std::size_t metric_depth_levels{10};
};

struct EngineApplyResult {
    order_book::ApplyResult book_result;
    std::optional<indicators::MarketMetrics> metrics;

    [[nodiscard]] bool ok() const noexcept { return book_result.ok(); }
};

class CalculationEngine {
public:
    explicit CalculationEngine(EngineConfig config = {});

    // Pre-registering is optional. A symbol is automatically created when its
    // first snapshot arrives.
    void register_symbol(const std::string& symbol);

    [[nodiscard]] EngineApplyResult apply(
        const market_data::RoutedMarketEvent& event);

    [[nodiscard]] const order_book::ShadowBook* book(
        const std::string& symbol) const noexcept;
    [[nodiscard]] order_book::ShadowBook* book(
        const std::string& symbol) noexcept;

    [[nodiscard]] std::optional<indicators::MarketMetrics> latest_metrics(
        const std::string& symbol) const;

    [[nodiscard]] std::vector<std::string> symbols() const;
    [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }

private:
    [[nodiscard]] order_book::ShadowBook& ensure_book(const std::string& symbol);

    EngineConfig config_;
    indicators::MarketMetricsCalculator metrics_calculator_;
    std::unordered_map<std::string, std::unique_ptr<order_book::ShadowBook>> books_;
    std::unordered_set<std::string> initialized_symbols_;
    std::unordered_map<std::string, indicators::MarketMetrics> latest_metrics_;
};

}  // namespace calculation_engine::engine
