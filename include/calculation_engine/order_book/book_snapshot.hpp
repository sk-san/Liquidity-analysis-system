#pragma once

#include "calculation_engine/market_data/market_event.hpp"
#include "calculation_engine/order_book/shadow_book.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace calculation_engine::order_book {

struct BookSnapshot {
    std::string symbol;
    market_data::Sequence sequence{};
    bool synchronized{};
    TopOfBook top;
    std::vector<LevelView> bids;
    std::vector<LevelView> asks;
    TradeStats trades;
    std::uint64_t visible_checksum{};
};

[[nodiscard]] inline BookSnapshot capture(
    const ShadowBook& book,
    std::size_t depth,
    bool include_hidden = false) {
    return BookSnapshot{
        book.symbol(),
        book.last_sequence(),
        book.synchronized(),
        book.top(),
        book.l3(market_data::Side::Bid, depth, include_hidden),
        book.l3(market_data::Side::Ask, depth, include_hidden),
        book.trade_stats(),
        book.visible_checksum()};
}

}  // namespace calculation_engine::order_book
