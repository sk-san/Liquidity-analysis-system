#include "calculation_engine/engine/calculation_engine.hpp"
#include "calculation_engine/transport/market_data_decoder.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>

namespace ce = calculation_engine;

namespace {

ce::market_data::OrderImage make_order(
    ce::market_data::EntryId entry_id,
    ce::market_data::Side side,
    ce::market_data::Price price,
    ce::market_data::Quantity quantity,
    ce::market_data::TimestampNs time) {
    return ce::market_data::OrderImage{
        entry_id,
        entry_id,
        42,
        time,
        "ABM",
        side,
        price,
        quantity,
        ce::market_data::Visibility::Visible,
        false};
}

void require(const ce::engine::EngineApplyResult& result) {
    if (!result.ok()) {
        std::cerr << "event rejected: " << result.book_result.message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    ce::engine::CalculationEngine engine;
    // require(engine.apply({
    //     "ABM",
    //     10'000,
    //     ce::market_data::SnapshotEvent{100, 1'000, "ABM", {}, std::nullopt}}));

    // require(engine.apply({
    //     "ABM",
    //     10'001,
    //     ce::market_data::AddEvent{
    //         101, 1'001, make_order(1, ce::market_data::Side::Bid, 276450, 10, 1'001)}}));
    // require(engine.apply({
    //     "ABM",
    //     10'002,
    //     ce::market_data::AddEvent{
    //         102, 1'002, make_order(2, ce::market_data::Side::Ask, 276500, 12, 1'002)}}));
    // require(engine.apply({
    //     "ABM",
    //     10'003,
    //     ce::market_data::ExecuteEvent{
    //         103, 1'003, 2, std::nullopt, 276500, 4, 8, ce::market_data::Side::Bid}}));

    const auto metrics = engine.latest_metrics("ABM");
    if (!metrics.has_value()) {
        std::cerr << "metrics unavailable\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(2)
              << "symbol=" << metrics->symbol << '\n'
              << "sequence=" << metrics->sequence << '\n'
              << "best_bid=" << metrics->best_bid_price.value_or(0)
              << " x " << metrics->best_bid_quantity.value_or(0) << '\n'
              << "best_ask=" << metrics->best_ask_price.value_or(0)
              << " x " << metrics->best_ask_quantity.value_or(0) << '\n'
              << "spread=" << metrics->quoted_spread.value_or(0) << '\n'
              << "midprice=" << metrics->midprice.value_or(0.0) << '\n'
              << "microprice=" << metrics->microprice.value_or(0.0) << '\n'
              << "imbalance=" << metrics->top_of_book_imbalance.value_or(0.0) << '\n'
              << "traded_volume=" << metrics->traded_volume << '\n';
            //   << "bid_liquidity_provision_ratio=" << metrics->bid_liquidity_provision_ratio << '\n';
            //   << "ask_liquidity_provision_ratioe=" << metrics->ask_liquidity_provision_ratio << '\n';
            //   << "is_market_maker" << metrics->is_market_maker << '\n';
    return 0;
}
