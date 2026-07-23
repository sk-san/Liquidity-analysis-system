#include "calculation_engine/market_data/market_event.hpp"
#include "calculation_engine/order_book/book_snapshot.hpp"
#include "calculation_engine/order_book/shadow_book.hpp"

#include <cassert>
#include <iostream>
#include <optional>

namespace ce = calculation_engine;

int main() {
    ce::order_book::ShadowBook book("TEST");

    ce::market_data::OrderImage bid{
        1,
        1,
        9,
        1,
        "TEST",
        ce::market_data::Side::Bid,
        100,
        10,
        ce::market_data::Visibility::Visible,
        false};
    ce::market_data::OrderImage ask{
        2,
        2,
        10,
        2,
        "TEST",
        ce::market_data::Side::Ask,
        101,
        12,
        ce::market_data::Visibility::Visible,
        false};

    assert(book.apply(ce::market_data::SnapshotEvent{
        10, 10, "TEST", {bid, ask}, std::nullopt}).ok());

    const auto snapshot = ce::order_book::capture(book, 5);
    assert(snapshot.symbol == "TEST");
    assert(snapshot.sequence == 10);
    assert(snapshot.synchronized);
    assert(snapshot.top.bid->first == 100);
    assert(snapshot.top.ask->first == 101);
    assert(snapshot.bids.size() == 1);
    assert(snapshot.asks.size() == 1);

    std::cout << "shadow book facade test passed\n";
    return 0;
}
