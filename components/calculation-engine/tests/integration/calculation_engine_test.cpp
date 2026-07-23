#include "calculation_engine/engine/calculation_engine.hpp"

#include <cassert>
#include <iostream>
#include <optional>

namespace ce = calculation_engine;

namespace {

ce::market_data::OrderImage order(
    ce::market_data::EntryId id,
    const std::string& symbol,
    ce::market_data::Side side,
    ce::market_data::Price price,
    ce::market_data::Quantity quantity) {
    return ce::market_data::OrderImage{
        id,
        id,
        11,
        static_cast<ce::market_data::TimestampNs>(id),
        symbol,
        side,
        price,
        quantity,
        ce::market_data::Visibility::Visible,
        false};
}

}  // namespace

int main() {
    ce::engine::CalculationEngine engine;

    // Incrementals are rejected until the initial authoritative snapshot.
    const auto before_snapshot = engine.apply({
        "TOPIX",
        1,
        ce::market_data::AddEvent{
            1, 1, order(1, "TOPIX", ce::market_data::Side::Bid, 100, 10)}});
    assert(before_snapshot.book_result.code == ce::order_book::ApplyCode::Rejected);

    auto result = engine.apply({
        "TOPIX",
        2,
        ce::market_data::SnapshotEvent{10, 10, "TOPIX", {}, std::nullopt}});
    assert(result.ok());

    result = engine.apply({
        "TOPIX",
        3,
        ce::market_data::AddEvent{
            11, 11, order(1, "TOPIX", ce::market_data::Side::Bid, 100, 10)}});
    assert(result.ok());

    result = engine.apply({
        "TOPIX",
        4,
        ce::market_data::AddEvent{
            12, 12, order(2, "TOPIX", ce::market_data::Side::Ask, 102, 20)}});
    assert(result.ok());
    assert(result.metrics->quoted_spread == 2);

    result = engine.apply({
        "TOPIX",
        5,
        ce::market_data::ExecuteEvent{
            13, 13, 2, std::nullopt, 102, 5, 15, ce::market_data::Side::Bid}});
    assert(result.ok());
    assert(result.metrics->traded_volume == 5);
    assert(result.metrics->best_ask_quantity == 15);

    // A gap invalidates current metrics until a snapshot recovers the book.
    result = engine.apply({
        "TOPIX",
        6,
        ce::market_data::ModifyEvent{15, 15, 1, 8}});
    assert(result.book_result.code == ce::order_book::ApplyCode::GapDetected);
    assert(result.metrics.has_value());
    assert(!result.metrics->synchronized);

    result = engine.apply({
        "TOPIX",
        7,
        ce::market_data::SnapshotEvent{15, 15, "TOPIX", {
            order(1, "TOPIX", ce::market_data::Side::Bid, 100, 8),
            order(2, "TOPIX", ce::market_data::Side::Ask, 102, 15)}, 102}});
    assert(result.ok());
    assert(result.metrics->synchronized);

    // A second symbol owns an independent sequence and book state.
    result = engine.apply({
        "BANKS",
        8,
        ce::market_data::SnapshotEvent{50, 50, "BANKS", {
            order(100, "BANKS", ce::market_data::Side::Bid, 300, 4),
            order(101, "BANKS", ce::market_data::Side::Ask, 301, 5)}, std::nullopt}});
    assert(result.ok());
    assert(engine.symbols().size() == 2);
    assert(engine.book("BANKS") != nullptr);
    assert(engine.latest_metrics("BANKS")->quoted_spread == 1);

    std::cout << "calculation engine integration test passed\n";
    return 0;
}
