#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace abides::shadow {

using Sequence = std::uint64_t;
using TimestampNs = std::int64_t;
using EntryId = std::uint64_t;
using OrderId = std::uint64_t;
using AgentId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;

enum class Side : std::uint8_t { Bid, Ask };
enum class Visibility : std::uint8_t { Visible, Hidden };

[[nodiscard]] constexpr const char* to_string(Side side) noexcept {
    return side == Side::Bid ? "BID" : "ASK";
}

[[nodiscard]] constexpr const char* to_string(Visibility visibility) noexcept {
    return visibility == Visibility::Visible ? "VISIBLE" : "HIDDEN";
}

// entry_id uniquely identifies a physical book entry.  For ordinary orders it
// can equal order_id.  It is deliberately separate because ABIDES price-to-
// comply orders can create visible and hidden halves related to one logical
// order_id.
struct OrderImage {
    EntryId entry_id{};
    OrderId order_id{};
    AgentId agent_id{};
    TimestampNs priority_time_ns{};
    std::string symbol;
    Side side{Side::Bid};
    Price price{};
    Quantity quantity{};
    Visibility visibility{Visibility::Visible};
    bool insert_by_id{false};
    bool is_market_maker{false};
};

struct AddEvent {
    Sequence sequence{};
    TimestampNs exchange_time_ns{};
    OrderImage order;
};

struct DeleteEvent {
    Sequence sequence{};
    TimestampNs exchange_time_ns{};
    EntryId entry_id{};
};

struct PartialCancelEvent {
    Sequence sequence{};
    TimestampNs exchange_time_ns{};
    EntryId entry_id{};
    Quantity cancelled_quantity{};
    std::optional<Quantity> remaining_quantity;
};

struct ModifyEvent {
    Sequence sequence{};
    TimestampNs exchange_time_ns{};
    EntryId entry_id{};
    Quantity new_quantity{};
};

struct ReplaceEvent {
    Sequence sequence{};
    TimestampNs exchange_time_ns{};
    EntryId old_entry_id{};
    OrderImage replacement;
};

// This event applies a trade already decided by ABIDES.  The shadow book does
// not choose a counterparty, price, or execution quantity.
struct ExecuteEvent {
    Sequence sequence{};
    TimestampNs exchange_time_ns{};
    EntryId passive_entry_id{};
    std::optional<EntryId> aggressor_entry_id;
    Price execution_price{};
    Quantity executed_quantity{};
    std::optional<Quantity> passive_remaining_quantity;
    Side aggressor_side{Side::Bid};
};

// A full L3 image used for startup and gap recovery.  snapshot_sequence is the
// last incremental sequence included in the image.
struct SnapshotEvent {
    Sequence snapshot_sequence{};
    TimestampNs exchange_time_ns{};
    std::string symbol;
    std::vector<OrderImage> orders;
    std::optional<Price> last_trade_price;
};

using MarketEvent = std::variant<
    AddEvent,
    DeleteEvent,
    PartialCancelEvent,
    ModifyEvent,
    ReplaceEvent,
    ExecuteEvent,
    SnapshotEvent>;

[[nodiscard]] inline Sequence event_sequence(const MarketEvent& event) {
    return std::visit(
        [](const auto& value) -> Sequence {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, SnapshotEvent>) {
                return value.snapshot_sequence;
            } else {
                return value.sequence;
            }
        },
        event);
}

}  // namespace abides::shadow
