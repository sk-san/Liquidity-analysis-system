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
    bool insert_by_id = false) {
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
        insert_by_id};
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

}  // namespace

int main() {
    test_fifo_and_modify_priority();
    test_execution_and_delete();
    test_gap_and_snapshot_recovery();
    test_hidden_and_ptc_entry_identity();
    test_replace_and_validation();
    std::cout << "all shadow-book tests passed\n";
    return 0;
}
