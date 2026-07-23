#pragma once

#include "shadow_book/events.hpp"

#include <string>
#include <type_traits>
#include <variant>

namespace calculation_engine::market_data {

using Sequence = abides::shadow::Sequence;
using TimestampNs = abides::shadow::TimestampNs;
using EntryId = abides::shadow::EntryId;
using OrderId = abides::shadow::OrderId;
using AgentId = abides::shadow::AgentId;
using Price = abides::shadow::Price;
using Quantity = abides::shadow::Quantity;
using Side = abides::shadow::Side;
using Visibility = abides::shadow::Visibility;
using OrderImage = abides::shadow::OrderImage;
using AddEvent = abides::shadow::AddEvent;
using DeleteEvent = abides::shadow::DeleteEvent;
using PartialCancelEvent = abides::shadow::PartialCancelEvent;
using ModifyEvent = abides::shadow::ModifyEvent;
using ReplaceEvent = abides::shadow::ReplaceEvent;
using ExecuteEvent = abides::shadow::ExecuteEvent;
using SnapshotEvent = abides::shadow::SnapshotEvent;
using MarketEvent = abides::shadow::MarketEvent;

// Transport-level envelope. The symbol is carried outside the payload because
// order-deletion and execution events identify entries but do not contain a
// symbol in the original shadow-book event contract.
struct RoutedMarketEvent {
    std::string symbol;
    TimestampNs received_wall_time_ns{};
    MarketEvent payload;
};

[[nodiscard]] inline Sequence event_sequence(const MarketEvent& event) {
    return abides::shadow::event_sequence(event);
}

[[nodiscard]] inline TimestampNs exchange_time_ns(const MarketEvent& event) {
    return std::visit(
        [](const auto& value) -> TimestampNs { return value.exchange_time_ns; },
        event);
}

[[nodiscard]] inline bool is_snapshot(const MarketEvent& event) noexcept {
    return std::holds_alternative<SnapshotEvent>(event);
}

}  // namespace calculation_engine::market_data
