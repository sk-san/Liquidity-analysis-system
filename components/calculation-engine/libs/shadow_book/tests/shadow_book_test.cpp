#include "shadow_book/shadow_book.hpp"

#include <cassert>
#include <iostream>

using namespace abides::shadow;

namespace {

OrderImage make_order(
    EntryId entry_id,
    OrderId order_id,
    Side side,
    Price price,
    Quantity quantity,
    TimestampNs time,
    Visibility visibility = Visibility::Visible,
    bool insert_by_id = false,
    bool is_market_maker = false) {
    return OrderImage{
        entry_id,
        order_id,
        7,
        time,
        "TEST",
        side,
        price,
        quantity,
        visibility,
        insert_by_id,
        is_market_maker};
}

void must_apply(ShadowBook& book, MarketEvent event) {
    const auto result = book.apply(event);
    if (!result.ok()) {
        std::cerr << result.message << '\n';
    }
    assert(result.ok());
}

void test_fifo_and_modify_priority() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{10, 0, "TEST", {}, std::nullopt});
    must_apply(book, AddEvent{11, 1, make_order(1, 101, Side::Bid, 100, 10, 1)});
    must_apply(book, AddEvent{12, 2, make_order(2, 102, Side::Bid, 100, 20, 2)});

    auto level = book.l3(Side::Bid, 1).front();
    assert((level.visible_fifo == std::vector<EntryId>{1, 2}));
    assert(level.visible_quantity == 30);

    // Reduction retains priority.
    must_apply(book, ModifyEvent{13, 3, 1, 8});
    level = book.l3(Side::Bid, 1).front();
    assert((level.visible_fifo == std::vector<EntryId>{1, 2}));
    assert(level.visible_quantity == 28);

    // Increase moves the order to the back, matching ABIDES PriceLevel.
    must_apply(book, ModifyEvent{14, 4, 1, 15});
    level = book.l3(Side::Bid, 1).front();
    assert((level.visible_fifo == std::vector<EntryId>{2, 1}));
    assert(level.visible_quantity == 35);
}

void test_execution_and_delete() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{20, 0, "TEST", {
        make_order(3, 103, Side::Ask, 101, 9, 1)}, std::nullopt});

    must_apply(book, ExecuteEvent{21, 1, 3, std::nullopt, 101, 4, 5, Side::Bid});
    assert(book.find(3)->quantity == 5);
    assert(book.trade_stats().total_volume == 4);

    must_apply(book, ExecuteEvent{22, 2, 3, std::nullopt, 101, 5, 0, Side::Bid});
    assert(!book.find(3).has_value());
    assert(book.order_count() == 0);
    assert(book.trade_stats().total_volume == 9);
}

void test_gap_and_snapshot_recovery() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{30, 0, "TEST", {}, std::nullopt});
    const auto gap = book.apply(AddEvent{32, 1, make_order(1, 1, Side::Bid, 99, 1, 1)});
    assert(gap.code == ApplyCode::GapDetected);
    assert(!book.synchronized());

    const auto blocked = book.apply(AddEvent{33, 2, make_order(2, 2, Side::Ask, 101, 1, 2)});
    assert(blocked.code == ApplyCode::Desynchronized);

    must_apply(book, SnapshotEvent{33, 2, "TEST", {
        make_order(1, 1, Side::Bid, 99, 1, 1),
        make_order(2, 2, Side::Ask, 101, 1, 2)}, std::nullopt});
    assert(book.synchronized());
    assert(book.last_sequence() == 33);
}

void test_hidden_and_ptc_entry_identity() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{40, 0, "TEST", {}, std::nullopt});

    // Same logical order_id, distinct physical entry_id values.
    must_apply(book, AddEvent{41, 1,
        make_order(1001, 500, Side::Bid, 99, 4, 1, Visibility::Hidden)});
    must_apply(book, AddEvent{42, 2,
        make_order(1002, 500, Side::Bid, 100, 4, 1, Visibility::Visible)});

    assert(book.order_count() == 2);
    assert(book.l2(Side::Bid, 2).front().price == 100);
    assert(book.l3(Side::Bid, 2, true).size() == 2);
}

void test_replace_and_validation() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{50, 0, "TEST", {
        make_order(1, 1, Side::Bid, 99, 3, 1)}, std::nullopt});
    must_apply(book, ReplaceEvent{51, 1, 1,
        make_order(2, 2, Side::Bid, 100, 5, 2)});
    assert(!book.find(1));
    assert(book.find(2)->price == 100);
    assert(book.validate().empty());
}

void test_change_flags_track_the_last_event() {
    ShadowBook book("TEST");
    assert(!book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    must_apply(book, SnapshotEvent{60, 0, "TEST", {}, std::nullopt});
    assert(!book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    must_apply(book, AddEvent{61, 1, make_order(1, 101, Side::Bid, 100, 10, 1)});
    assert(book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    must_apply(book, AddEvent{62, 2, make_order(2, 102, Side::Ask, 101, 8, 2)});
    assert(!book.is_bid_orderbook_changed);
    assert(book.is_ask_orderbook_changed);

    must_apply(book, ModifyEvent{63, 3, 1, 7});
    assert(book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    // An accepted no-op resets the flags without reporting a false change.
    must_apply(book, ModifyEvent{64, 4, 1, 7});
    assert(!book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    must_apply(book, PartialCancelEvent{65, 5, 2, 2, 6});
    assert(!book.is_bid_orderbook_changed);
    assert(book.is_ask_orderbook_changed);

    must_apply(book, ExecuteEvent{66, 6, 2, std::nullopt, 101, 2, 4, Side::Bid});
    assert(!book.is_bid_orderbook_changed);
    assert(book.is_ask_orderbook_changed);

    must_apply(book, DeleteEvent{67, 7, 1});
    assert(book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    const auto rejected = book.apply(DeleteEvent{68, 8, 999});
    assert(rejected.code == ApplyCode::Rejected);
    assert(!book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    const auto duplicate = book.apply(ModifyEvent{67, 9, 2, 3});
    assert(duplicate.code == ApplyCode::Duplicate);
    assert(!book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    must_apply(book, SnapshotEvent{67, 10, "TEST", {
        make_order(2, 102, Side::Ask, 101, 4, 2)}, std::nullopt});
    assert(!book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);

    must_apply(book, SnapshotEvent{68, 11, "TEST", {
        make_order(3, 103, Side::Bid, 99, 5, 3),
        make_order(2, 102, Side::Ask, 101, 4, 2)}, std::nullopt});
    assert(book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);
}

void test_market_maker_visible_quantity_lifecycle() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{70, 0, "TEST", {
        make_order(
            1, 101, Side::Bid, 100, 4, 1,
            Visibility::Visible, false, true),
        make_order(2, 102, Side::Bid, 100, 6, 2),
        make_order(
            3, 103, Side::Bid, 100, 20, 3,
            Visibility::Hidden, false, true)}, std::nullopt});

    auto level = book.l2(Side::Bid, 1).front();
    assert(level.visible_quantity == 10);
    assert(level.visible_mm_quantity == 4);
    assert(book.validate().empty());

    must_apply(book, ModifyEvent{71, 4, 1, 7});
    level = book.l2(Side::Bid, 1).front();
    assert(level.visible_quantity == 13);
    assert(level.visible_mm_quantity == 7);

    must_apply(book, PartialCancelEvent{72, 5, 2, 2, 4});
    level = book.l2(Side::Bid, 1).front();
    assert(level.visible_quantity == 11);
    assert(level.visible_mm_quantity == 7);

    must_apply(book, DeleteEvent{73, 6, 1});
    level = book.l2(Side::Bid, 1).front();
    assert(level.visible_quantity == 4);
    assert(level.visible_mm_quantity == 0);
    assert(book.validate().empty());
}

void test_snapshot_change_detection_includes_hidden_fifo() {
    ShadowBook book("TEST");
    must_apply(book, SnapshotEvent{80, 0, "TEST", {
        make_order(
            1, 101, Side::Bid, 100, 5, 1,
            Visibility::Hidden)}, std::nullopt});

    must_apply(book, SnapshotEvent{81, 1, "TEST", {
        make_order(
            2, 102, Side::Bid, 100, 5, 1,
            Visibility::Hidden)}, std::nullopt});
    assert(book.is_bid_orderbook_changed);
    assert(!book.is_ask_orderbook_changed);
}

}  // namespace

int main() {
    test_fifo_and_modify_priority();
    test_execution_and_delete();
    test_gap_and_snapshot_recovery();
    test_hidden_and_ptc_entry_identity();
    test_replace_and_validation();
    test_change_flags_track_the_last_event();
    test_market_maker_visible_quantity_lifecycle();
    test_snapshot_change_detection_includes_hidden_fifo();
    std::cout << "all shadow-book tests passed\n";
    return 0;
}
