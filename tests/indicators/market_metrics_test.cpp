#include "calculation_engine/indicators/market_metrics.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace ce = calculation_engine;

namespace {

ce::market_data::OrderImage order(
    ce::market_data::EntryId id,
    ce::market_data::Side side,
    ce::market_data::Price price,
    ce::market_data::Quantity quantity) {
    return ce::market_data::OrderImage{
        id,
        id,
        7,
        static_cast<ce::market_data::TimestampNs>(id),
        "TEST",
        side,
        price,
        quantity,
        ce::market_data::Visibility::Visible,
        false};
}

bool near(double lhs, double rhs) {
    return std::fabs(lhs - rhs) < 1e-9;
}

}  // namespace

int main() {
    ce::order_book::ShadowBook book("TEST");
    assert(book.apply(ce::market_data::SnapshotEvent{10, 0, "TEST", {
        order(1, ce::market_data::Side::Bid, 100, 10),
        order(2, ce::market_data::Side::Ask, 102, 30),
        order(3, ce::market_data::Side::Bid, 99, 5),
        order(4, ce::market_data::Side::Ask, 103, 7)}, std::nullopt}).ok());

    ce::indicators::MarketMetricsCalculator calculator(2);
    const auto metrics = calculator.calculate(book, 123);

    assert(metrics.synchronized);
    assert(metrics.sequence == 10);
    assert(metrics.best_bid_price == 100);
    assert(metrics.best_bid_quantity == 10);
    assert(metrics.best_ask_price == 102);
    assert(metrics.best_ask_quantity == 30);
    assert(metrics.quoted_spread == 2);
    assert(near(*metrics.midprice, 101.0));
    assert(near(*metrics.microprice, 100.5));
    assert(near(*metrics.top_of_book_imbalance, -0.5));
    assert(metrics.bid_visible_depth == 15);
    assert(metrics.ask_visible_depth == 37);

    std::cout << "market metrics test passed\n";
    return 0;
}
