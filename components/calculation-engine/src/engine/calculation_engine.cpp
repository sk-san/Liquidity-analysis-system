#include "calculation_engine/engine/calculation_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace calculation_engine::engine {
namespace {

order_book::ApplyResult rejected(std::string message) {
    return {order_book::ApplyCode::Rejected, std::move(message)};
}

}  // namespace

CalculationEngine::CalculationEngine(EngineConfig config)
    : config_(config), metrics_calculator_(config.metric_depth_levels) {}

void CalculationEngine::register_symbol(const std::string& symbol) {
    if (symbol.empty()) {
        throw std::invalid_argument("symbol must not be empty");
    }
    (void)ensure_book(symbol);
}

order_book::ShadowBook& CalculationEngine::ensure_book(const std::string& symbol) {
    if (symbol.empty()) {
        throw std::invalid_argument("event symbol must not be empty");
    }
    auto found = books_.find(symbol);
    if (found != books_.end()) {
        return *found->second;
    }

    auto inserted = books_.emplace(
        symbol,
        std::make_unique<order_book::ShadowBook>(symbol, config_.strict_sequence));
    return *inserted.first->second;
}

EngineApplyResult CalculationEngine::apply(
    const market_data::RoutedMarketEvent& event) {
    if (event.symbol.empty()) {
        return {rejected("routed event symbol must not be empty"), std::nullopt};
    }

    auto& target_book = ensure_book(event.symbol);
    const bool snapshot = market_data::is_snapshot(event.payload);
    const bool initialized = initialized_symbols_.find(event.symbol) !=
        initialized_symbols_.end();

    if (config_.require_initial_snapshot && !initialized && !snapshot) {
        return {
            rejected("initial snapshot required before incremental events"),
            std::nullopt};
    }

    auto apply_result = target_book.apply(event.payload);
    if (apply_result.code == order_book::ApplyCode::Applied && snapshot) {
        initialized_symbols_.insert(event.symbol);
    }

    std::optional<indicators::MarketMetrics> metrics;
    if (apply_result.code != order_book::ApplyCode::Rejected) {
        metrics = metrics_calculator_.calculate(
            target_book,
            market_data::exchange_time_ns(event.payload));
        latest_metrics_[event.symbol] = *metrics;
    }

    return {std::move(apply_result), std::move(metrics)};
}

const order_book::ShadowBook* CalculationEngine::book(
    const std::string& symbol) const noexcept {
    const auto found = books_.find(symbol);
    return found == books_.end() ? nullptr : found->second.get();
}

order_book::ShadowBook* CalculationEngine::book(
    const std::string& symbol) noexcept {
    const auto found = books_.find(symbol);
    return found == books_.end() ? nullptr : found->second.get();
}

std::optional<indicators::MarketMetrics> CalculationEngine::latest_metrics(
    const std::string& symbol) const {
    const auto found = latest_metrics_.find(symbol);
    if (found == latest_metrics_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<std::string> CalculationEngine::symbols() const {
    std::vector<std::string> result;
    result.reserve(books_.size());
    for (const auto& [symbol, ignored] : books_) {
        (void)ignored;
        result.push_back(symbol);
    }
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace calculation_engine::engine
