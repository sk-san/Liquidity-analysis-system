#include "shadow_book/shadow_book.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace abides::shadow {
namespace {

ApplyResult applied(std::string message = {}) {
    return {ApplyCode::Applied, std::move(message)};
}

ApplyResult rejected(std::string message) {
    return {ApplyCode::Rejected, std::move(message)};
}

void fnv_mix(std::uint64_t& hash, const void* data, std::size_t size) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= prime;
    }
}

template <typename T>
void fnv_mix_value(std::uint64_t& hash, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    fnv_mix(hash, &value, sizeof(T));
}

}  // namespace

ShadowBook::ShadowBook(std::string symbol, bool strict_sequence)
    : symbol_(std::move(symbol)), strict_sequence_(strict_sequence) {
    if (symbol_.empty()) {
        throw std::invalid_argument("symbol must not be empty");
    }
}

ApplyResult ShadowBook::apply(const MarketEvent& event) {
    return std::visit(
        [this](const auto& value) -> ApplyResult {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, SnapshotEvent>) {
                return apply_snapshot(value);
            } else {
                const auto sequence_result = check_incremental_sequence(value.sequence);
                if (sequence_result.code != ApplyCode::Applied) {
                    return sequence_result;
                }
                auto result = apply_incremental(value);
                if (result.code == ApplyCode::Applied) {
                    commit_sequence(value.sequence);
                }
                return result;
            }
        },
        event);
}

ApplyResult ShadowBook::check_incremental_sequence(Sequence sequence) {
    if (!synchronized_) {
        return {ApplyCode::Desynchronized, "book requires a fresh snapshot"};
    }
    if (!strict_sequence_) {
        if (sequence <= last_sequence_) {
            return {ApplyCode::Duplicate, "event sequence already applied"};
        }
        return applied();
    }
    if (sequence <= last_sequence_) {
        return {ApplyCode::Duplicate, "event sequence already applied"};
    }
    if (last_sequence_ != 0 && sequence != last_sequence_ + 1) {
        synchronized_ = false;
        std::ostringstream out;
        out << "sequence gap: expected " << (last_sequence_ + 1) << ", got " << sequence;
        return {ApplyCode::GapDetected, out.str()};
    }
    return applied();
}

void ShadowBook::commit_sequence(Sequence sequence) noexcept {
    last_sequence_ = sequence;
}

ApplyResult ShadowBook::apply_snapshot(const SnapshotEvent& event) {
    if (event.symbol != symbol_) {
        return rejected("snapshot symbol does not match shadow book");
    }

    ShadowBook replacement(symbol_, strict_sequence_);
    replacement.last_sequence_ = event.snapshot_sequence;
    replacement.synchronized_ = true;
    replacement.trade_stats_.last_trade_price = event.last_trade_price;

    for (const auto& order : event.orders) {
        auto result = replacement.add_order(order);
        if (result.code != ApplyCode::Applied) {
            return rejected("invalid snapshot: " + result.message);
        }
    }

    auto errors = replacement.validate();
    if (!errors.empty()) {
        return rejected("invalid snapshot invariant: " + errors.front());
    }

    bids_ = std::move(replacement.bids_);
    asks_ = std::move(replacement.asks_);
    orders_ = std::move(replacement.orders_);
    trade_stats_ = replacement.trade_stats_;
    last_sequence_ = event.snapshot_sequence;
    synchronized_ = true;
    return applied("snapshot installed");
}

ApplyResult ShadowBook::apply_incremental(const AddEvent& event) {
    if (event.order.symbol != symbol_) {
        return rejected("add symbol does not match shadow book");
    }
    return add_order(event.order);
}

ApplyResult ShadowBook::apply_incremental(const DeleteEvent& event) {
    return delete_order(event.entry_id);
}

ApplyResult ShadowBook::apply_incremental(const PartialCancelEvent& event) {
    if (event.cancelled_quantity <= 0) {
        return rejected("partial-cancel quantity must be positive");
    }
    return apply_quantity_delta(
        event.entry_id,
        event.cancelled_quantity,
        event.remaining_quantity);
}

ApplyResult ShadowBook::apply_incremental(const ModifyEvent& event) {
    return set_quantity(event.entry_id, event.new_quantity);
}

ApplyResult ShadowBook::apply_incremental(const ReplaceEvent& event) {
    if (event.replacement.symbol != symbol_) {
        return rejected("replacement symbol does not match shadow book");
    }
    if (orders_.find(event.old_entry_id) == orders_.end()) {
        return rejected("replace target does not exist");
    }
    if (event.replacement.entry_id != event.old_entry_id &&
        orders_.find(event.replacement.entry_id) != orders_.end()) {
        return rejected("replacement entry_id already exists");
    }

    const auto old = orders_.at(event.old_entry_id).image;
    auto delete_result = delete_order(event.old_entry_id);
    if (delete_result.code != ApplyCode::Applied) {
        return delete_result;
    }

    auto add_result = add_order(event.replacement);
    if (add_result.code == ApplyCode::Applied) {
        return add_result;
    }

    // Preserve atomic behavior for the read model if the replacement is invalid.
    const auto rollback = add_order(old);
    if (rollback.code != ApplyCode::Applied) {
        synchronized_ = false;
        return {ApplyCode::Desynchronized, "replace failed and rollback failed"};
    }
    return rejected("replacement rejected: " + add_result.message);
}

ApplyResult ShadowBook::apply_incremental(const ExecuteEvent& event) {
    if (event.executed_quantity <= 0) {
        return rejected("execution quantity must be positive");
    }
    if (event.execution_price < 0) {
        return rejected("execution price must be non-negative");
    }

    auto found = orders_.find(event.passive_entry_id);
    if (found == orders_.end()) {
        return rejected("passive execution entry does not exist");
    }
    const Side passive_side = found->second.image.side;
    if (passive_side == event.aggressor_side) {
        return rejected("aggressor side equals passive side");
    }

    auto result = apply_quantity_delta(
        event.passive_entry_id,
        event.executed_quantity,
        event.passive_remaining_quantity);
    if (result.code != ApplyCode::Applied) {
        return result;
    }

    trade_stats_.trade_count += 1;
    trade_stats_.total_volume += event.executed_quantity;
    trade_stats_.last_trade_price = event.execution_price;
    trade_stats_.last_trade_time_ns = event.exchange_time_ns;
    return applied();
}

ApplyResult ShadowBook::add_order(const OrderImage& order) {
    if (order.symbol != symbol_) {
        return rejected("order symbol does not match shadow book");
    }
    if (order.entry_id == 0) {
        return rejected("entry_id must be non-zero");
    }
    if (order.quantity <= 0) {
        return rejected("order quantity must be positive");
    }
    if (order.price < 0) {
        return rejected("order price must be non-negative");
    }
    if (orders_.find(order.entry_id) != orders_.end()) {
        return rejected("entry_id already exists");
    }

    auto& level = get_or_create_level(order.side, order.price);
    auto& queue = queue_for(level, order.visibility);

    std::list<EntryId>::iterator iterator;
    if (order.insert_by_id && order.visibility == Visibility::Visible) {
        iterator = queue.begin();
        while (iterator != queue.end()) {
            const auto existing = orders_.find(*iterator);
            if (existing == orders_.end()) {
                synchronized_ = false;
                return {ApplyCode::Desynchronized, "price queue references missing order"};
            }
            if (existing->second.image.order_id > order.order_id) {
                break;
            }
            ++iterator;
        }
        iterator = queue.insert(iterator, order.entry_id);
    } else {
        queue.push_back(order.entry_id);
        iterator = std::prev(queue.end());
    }

    quantity_for(level, order.visibility) += order.quantity;
    orders_.emplace(order.entry_id, OrderRecord{order, iterator});
    return applied();
}

ApplyResult ShadowBook::delete_order(EntryId entry_id) {
    auto found = orders_.find(entry_id);
    if (found == orders_.end()) {
        return rejected("delete target does not exist");
    }

    const auto image = found->second.image;
    auto* level = find_level(image.side, image.price);
    if (level == nullptr) {
        synchronized_ = false;
        return {ApplyCode::Desynchronized, "order exists without price level"};
    }

    auto& queue = queue_for(*level, image.visibility);
    queue.erase(found->second.queue_iterator);
    auto& level_quantity = quantity_for(*level, image.visibility);
    if (level_quantity < image.quantity) {
        synchronized_ = false;
        return {ApplyCode::Desynchronized, "price-level quantity underflow"};
    }
    level_quantity -= image.quantity;
    orders_.erase(found);
    erase_level_if_empty(image.side, image.price);
    return applied();
}

ApplyResult ShadowBook::set_quantity(EntryId entry_id, Quantity new_quantity) {
    if (new_quantity <= 0) {
        return rejected("modified quantity must be positive; use delete for zero");
    }

    auto found = orders_.find(entry_id);
    if (found == orders_.end()) {
        return rejected("modify target does not exist");
    }

    auto& record = found->second;
    auto* level = find_level(record.image.side, record.image.price);
    if (level == nullptr) {
        synchronized_ = false;
        return {ApplyCode::Desynchronized, "order exists without price level"};
    }

    auto& aggregate = quantity_for(*level, record.image.visibility);
    const Quantity old_quantity = record.image.quantity;
    const Quantity delta = new_quantity - old_quantity;
    if (delta < 0 && aggregate < -delta) {
        synchronized_ = false;
        return {ApplyCode::Desynchronized, "price-level quantity underflow"};
    }
    aggregate += delta;
    record.image.quantity = new_quantity;

    // Matches ABIDES PriceLevel.update_order_quantity(): reductions preserve
    // priority; increases move the order to the back of its visibility queue.
    if (new_quantity > old_quantity) {
        move_to_back(record, *level);
    }
    return applied();
}

ApplyResult ShadowBook::apply_quantity_delta(
    EntryId entry_id,
    Quantity decrement,
    std::optional<Quantity> authoritative_remaining) {
    auto found = orders_.find(entry_id);
    if (found == orders_.end()) {
        return rejected("quantity-update target does not exist");
    }
    if (decrement <= 0 || decrement > found->second.image.quantity) {
        return rejected("quantity decrement is outside current order quantity");
    }

    const Quantity computed = found->second.image.quantity - decrement;
    const Quantity remaining = authoritative_remaining.value_or(computed);
    if (remaining != computed) {
        std::ostringstream out;
        out << "authoritative remaining quantity " << remaining
            << " disagrees with computed value " << computed;
        synchronized_ = false;
        return {ApplyCode::Desynchronized, out.str()};
    }

    if (remaining == 0) {
        return delete_order(entry_id);
    }
    return set_quantity(entry_id, remaining);
}

ShadowBook::PriceLevel* ShadowBook::find_level(Side side, Price price) {
    if (side == Side::Bid) {
        auto found = bids_.find(price);
        return found == bids_.end() ? nullptr : &found->second;
    }
    auto found = asks_.find(price);
    return found == asks_.end() ? nullptr : &found->second;
}

const ShadowBook::PriceLevel* ShadowBook::find_level(Side side, Price price) const {
    if (side == Side::Bid) {
        auto found = bids_.find(price);
        return found == bids_.end() ? nullptr : &found->second;
    }
    auto found = asks_.find(price);
    return found == asks_.end() ? nullptr : &found->second;
}

ShadowBook::PriceLevel& ShadowBook::get_or_create_level(Side side, Price price) {
    if (side == Side::Bid) {
        return bids_.try_emplace(price, PriceLevel{price, {}, {}, 0, 0}).first->second;
    }
    return asks_.try_emplace(price, PriceLevel{price, {}, {}, 0, 0}).first->second;
}

void ShadowBook::erase_level_if_empty(Side side, Price price) {
    auto* level = find_level(side, price);
    if (level == nullptr || !level->visible.empty() || !level->hidden.empty()) {
        return;
    }
    if (side == Side::Bid) {
        bids_.erase(price);
    } else {
        asks_.erase(price);
    }
}

std::list<EntryId>& ShadowBook::queue_for(PriceLevel& level, Visibility visibility) {
    return visibility == Visibility::Visible ? level.visible : level.hidden;
}

const std::list<EntryId>& ShadowBook::queue_for(
    const PriceLevel& level,
    Visibility visibility) const {
    return visibility == Visibility::Visible ? level.visible : level.hidden;
}

Quantity& ShadowBook::quantity_for(PriceLevel& level, Visibility visibility) {
    return visibility == Visibility::Visible ? level.visible_quantity : level.hidden_quantity;
}

Quantity ShadowBook::quantity_for(
    const PriceLevel& level,
    Visibility visibility) const {
    return visibility == Visibility::Visible ? level.visible_quantity : level.hidden_quantity;
}

void ShadowBook::move_to_back(OrderRecord& record, PriceLevel& level) {
    auto& queue = queue_for(level, record.image.visibility);
    queue.erase(record.queue_iterator);
    queue.push_back(record.image.entry_id);
    record.queue_iterator = std::prev(queue.end());
}

TopOfBook ShadowBook::top() const {
    TopOfBook result;
    for (const auto& [price, level] : bids_) {
        if (level.visible_quantity > 0) {
            result.bid = std::make_pair(price, level.visible_quantity);
            break;
        }
    }
    for (const auto& [price, level] : asks_) {
        if (level.visible_quantity > 0) {
            result.ask = std::make_pair(price, level.visible_quantity);
            break;
        }
    }
    return result;
}

template <typename Levels>
std::vector<LevelView> ShadowBook::make_view(
    const Levels& levels,
    std::size_t depth,
    bool include_hidden) const {
    std::vector<LevelView> result;
    result.reserve(std::min(depth, levels.size()));
    for (const auto& [price, level] : levels) {
        if (result.size() >= depth) {
            break;
        }
        if (level.visible_quantity == 0 && (!include_hidden || level.hidden_quantity == 0)) {
            continue;
        }
        LevelView view;
        view.price = price;
        view.visible_quantity = level.visible_quantity;
        view.visible_fifo.assign(level.visible.begin(), level.visible.end());
        if (include_hidden) {
            view.hidden_quantity = level.hidden_quantity;
            view.hidden_fifo.assign(level.hidden.begin(), level.hidden.end());
        }
        result.push_back(std::move(view));
    }
    return result;
}

std::vector<LevelView> ShadowBook::l2(Side side, std::size_t depth) const {
    return side == Side::Bid
        ? make_view(bids_, depth, false)
        : make_view(asks_, depth, false);
}

std::vector<LevelView> ShadowBook::l3(
    Side side,
    std::size_t depth,
    bool include_hidden) const {
    return side == Side::Bid
        ? make_view(bids_, depth, include_hidden)
        : make_view(asks_, depth, include_hidden);
}

std::optional<OrderImage> ShadowBook::find(EntryId entry_id) const {
    auto found = orders_.find(entry_id);
    if (found == orders_.end()) {
        return std::nullopt;
    }
    return found->second.image;
}

Quantity ShadowBook::total_visible_quantity(Side side) const {
    Quantity total = 0;
    const auto add = [&total](const auto& levels) {
        for (const auto& [_, level] : levels) {
            total += level.visible_quantity;
        }
    };
    if (side == Side::Bid) {
        add(bids_);
    } else {
        add(asks_);
    }
    return total;
}

double ShadowBook::visible_imbalance() const {
    const auto bid = static_cast<double>(total_visible_quantity(Side::Bid));
    const auto ask = static_cast<double>(total_visible_quantity(Side::Ask));
    const double total = bid + ask;
    return total == 0.0 ? 0.0 : (bid - ask) / total;
}

std::uint64_t ShadowBook::visible_checksum() const {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix_levels = [this, &hash](Side side, const auto& levels) {
        const auto side_value = static_cast<std::uint8_t>(side);
        fnv_mix_value(hash, side_value);
        for (const auto& [price, level] : levels) {
            if (level.visible.empty()) {
                continue;
            }
            fnv_mix_value(hash, price);
            for (const auto entry_id : level.visible) {
                const auto found = orders_.find(entry_id);
                if (found == orders_.end()) {
                    continue;
                }
                fnv_mix_value(hash, found->second.image.entry_id);
                fnv_mix_value(hash, found->second.image.order_id);
                fnv_mix_value(hash, found->second.image.quantity);
            }
        }
    };
    mix_levels(Side::Bid, bids_);
    mix_levels(Side::Ask, asks_);
    return hash;
}

std::vector<std::string> ShadowBook::validate() const {
    std::vector<std::string> errors;
    std::unordered_map<EntryId, std::size_t> occurrences;

    const auto validate_levels = [this, &errors, &occurrences](Side side, const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (price != level.price) {
                errors.push_back("map key differs from stored level price");
            }
            Quantity visible_sum = 0;
            Quantity hidden_sum = 0;
            const auto validate_queue = [&](Visibility visibility, const auto& queue, Quantity& sum) {
                for (const auto entry_id : queue) {
                    ++occurrences[entry_id];
                    auto found = orders_.find(entry_id);
                    if (found == orders_.end()) {
                        errors.push_back("queue references unknown entry_id");
                        continue;
                    }
                    const auto& image = found->second.image;
                    if (image.side != side || image.price != price || image.visibility != visibility) {
                        errors.push_back("order metadata disagrees with containing price queue");
                    }
                    if (image.quantity <= 0) {
                        errors.push_back("order has non-positive quantity");
                    }
                    sum += image.quantity;
                }
            };
            validate_queue(Visibility::Visible, level.visible, visible_sum);
            validate_queue(Visibility::Hidden, level.hidden, hidden_sum);
            if (visible_sum != level.visible_quantity) {
                errors.push_back("visible aggregate quantity mismatch");
            }
            if (hidden_sum != level.hidden_quantity) {
                errors.push_back("hidden aggregate quantity mismatch");
            }
            if (level.visible.empty() && level.hidden.empty()) {
                errors.push_back("empty price level retained");
            }
        }
    };

    validate_levels(Side::Bid, bids_);
    validate_levels(Side::Ask, asks_);

    for (const auto& [entry_id, _] : orders_) {
        if (occurrences[entry_id] != 1) {
            errors.push_back("order is not represented exactly once in price queues");
        }
    }

    const auto top_of_book = top();
    if (top_of_book.bid && top_of_book.ask &&
        top_of_book.bid->first >= top_of_book.ask->first) {
        errors.push_back("visible book is crossed or locked");
    }
    return errors;
}

void ShadowBook::clear() {
    bids_.clear();
    asks_.clear();
    orders_.clear();
    trade_stats_ = {};
    last_sequence_ = 0;
    synchronized_ = true;
}

}  // namespace abides::shadow
