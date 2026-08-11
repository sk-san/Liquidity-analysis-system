#pragma once

#include "shadow_book/events.hpp"

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace abides::shadow {

struct LevelView {
    Price price{};
    Quantity visible_quantity{};
    Quantity visible_mm_quantity{};
    Quantity hidden_quantity{};
    std::vector<EntryId> visible_fifo;
    std::vector<EntryId> hidden_fifo;
};

struct TopOfBook {
    std::optional<std::pair<Price, Quantity>> bid;
    std::optional<std::pair<Price, Quantity>> ask;
};

struct TradeStats {
    std::uint64_t trade_count{};
    Quantity total_volume{};
    std::optional<Price> last_trade_price;
    std::optional<TimestampNs> last_trade_time_ns;
};

enum class ApplyCode {
    Applied,
    Duplicate,
    GapDetected,
    Desynchronized,
    Rejected
};

struct ApplyResult {
    ApplyCode code{ApplyCode::Rejected};
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return code == ApplyCode::Applied || code == ApplyCode::Duplicate;
    }
};

class ShadowBook {
public:
    explicit ShadowBook(std::string symbol, bool strict_sequence = true);

    [[nodiscard]] ApplyResult apply(const MarketEvent& event);

    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] bool synchronized() const noexcept { return synchronized_; }
    [[nodiscard]] Sequence last_sequence() const noexcept { return last_sequence_; }
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_.size(); }
    [[nodiscard]] const TradeStats& trade_stats() const noexcept { return trade_stats_; }

    [[nodiscard]] TopOfBook top() const;
    [[nodiscard]] std::vector<LevelView> l2(Side side, std::size_t depth) const;
    [[nodiscard]] std::vector<LevelView> l3(
        Side side,
        std::size_t depth,
        bool include_hidden = false) const;

    [[nodiscard]] std::optional<OrderImage> find(EntryId entry_id) const;
    [[nodiscard]] Quantity total_visible_quantity(Side side) const;
    [[nodiscard]] double visible_imbalance() const;

    // Deterministic FNV-1a hash over visible L3 state.  Useful for periodic
    // comparison with a checksum produced by the ABIDES-side publisher.
    [[nodiscard]] std::uint64_t visible_checksum() const;

    // Returns an empty vector when all structural invariants hold.
    [[nodiscard]] std::vector<std::string> validate() const;

    // Describe the visible/hidden side changes made by the most recent call to
    // apply(). Rejected, duplicate, and sequence-error events leave both false.
    bool is_bid_orderbook_changed{false};
    bool is_ask_orderbook_changed{false};

    void clear();

private:
    struct PriceLevel {
        Price price{};
        std::list<EntryId> visible;
        std::list<EntryId> hidden;
        Quantity visible_quantity{};
        Quantity hidden_quantity{};
        Quantity visible_mm_quantity{};
    };

    struct OrderRecord {
        OrderImage image;
        std::list<EntryId>::iterator queue_iterator;
    };

    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    [[nodiscard]] ApplyResult apply_snapshot(const SnapshotEvent& event);
    [[nodiscard]] ApplyResult apply_incremental(const AddEvent& event);
    [[nodiscard]] ApplyResult apply_incremental(const DeleteEvent& event);
    [[nodiscard]] ApplyResult apply_incremental(const PartialCancelEvent& event);
    [[nodiscard]] ApplyResult apply_incremental(const ModifyEvent& event);
    [[nodiscard]] ApplyResult apply_incremental(const ReplaceEvent& event);
    [[nodiscard]] ApplyResult apply_incremental(const ExecuteEvent& event);

    [[nodiscard]] ApplyResult add_order(const OrderImage& order);
    [[nodiscard]] ApplyResult delete_order(EntryId entry_id);
    [[nodiscard]] ApplyResult set_quantity(EntryId entry_id, Quantity new_quantity);
    [[nodiscard]] ApplyResult apply_quantity_delta(
        EntryId entry_id,
        Quantity decrement,
        std::optional<Quantity> authoritative_remaining);

    [[nodiscard]] PriceLevel* find_level(Side side, Price price);
    [[nodiscard]] const PriceLevel* find_level(Side side, Price price) const;
    [[nodiscard]] PriceLevel& get_or_create_level(Side side, Price price);
    void erase_level_if_empty(Side side, Price price);

    [[nodiscard]] std::list<EntryId>& queue_for(PriceLevel& level, Visibility visibility);
    [[nodiscard]] const std::list<EntryId>& queue_for(
        const PriceLevel& level,
        Visibility visibility) const;
    [[nodiscard]] Quantity& quantity_for(PriceLevel& level, Visibility visibility);
    [[nodiscard]] Quantity quantity_for(
        const PriceLevel& level,
        Visibility visibility) const;

    void move_to_back(OrderRecord& record, PriceLevel& level);
    void mark_side_changed(Side side) noexcept;
    [[nodiscard]] ApplyResult check_incremental_sequence(Sequence sequence);
    void commit_sequence(Sequence sequence) noexcept;

    template <typename Levels>
    [[nodiscard]] std::vector<LevelView> make_view(
        const Levels& levels,
        std::size_t depth,
        bool include_hidden) const;

    template <typename Levels>
    [[nodiscard]] bool is_order_updated(const Levels& source, const Levels& n_diff_source);

    std::string symbol_;
    bool strict_sequence_{true};
    bool synchronized_{true};
    Sequence last_sequence_{0};
    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<EntryId, OrderRecord> orders_;
    TradeStats trade_stats_;
};

}  // namespace abides::shadow
