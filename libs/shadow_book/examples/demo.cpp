#include "shadow_book/shadow_book.hpp"

#include <cstdlib>
#include <iostream>

using namespace abides::shadow;

namespace {

void require(const ApplyResult& result) {
    if (!result.ok()) {
        std::cerr << "apply failed: " << result.message << '\n';
        std::exit(1);
    }
}

OrderImage order(
    EntryId entry_id,
    Side side,
    Price price,
    Quantity quantity,
    TimestampNs time) {
    return OrderImage{
        entry_id,
        entry_id,
        42,
        time,
        "TOPIX",
        side,
        price,
        quantity,
        Visibility::Visible,
        false};
}

}  // namespace

int main() {
    ShadowBook book("TOPIX");

    require(book.apply(SnapshotEvent{100, 1'000, "TOPIX", {}, std::nullopt}));
    require(book.apply(AddEvent{101, 1'001, order(1, Side::Bid, 276450, 10, 1'001)}));
    require(book.apply(AddEvent{102, 1'002, order(2, Side::Bid, 276450, 5, 1'002)}));
    require(book.apply(AddEvent{103, 1'003, order(3, Side::Ask, 276500, 12, 1'003)}));

    // ABIDES has already decided that passive ask entry 3 traded for four.
    require(book.apply(ExecuteEvent{
        104,
        1'004,
        3,
        std::nullopt,
        276500,
        4,
        8,
        Side::Bid}));

    const auto top = book.top();
    std::cout << "best bid: " << top.bid->first << " x " << top.bid->second << '\n';
    std::cout << "best ask: " << top.ask->first << " x " << top.ask->second << '\n';
    std::cout << "visible imbalance: " << book.visible_imbalance() << '\n';
    std::cout << "checksum: " << book.visible_checksum() << '\n';

    const auto errors = book.validate();
    if (!errors.empty()) {
        std::cerr << "invalid book: " << errors.front() << '\n';
        return 1;
    }
    return 0;
}
