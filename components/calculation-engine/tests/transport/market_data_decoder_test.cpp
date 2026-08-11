#include "calculation_engine/engine/calculation_engine.hpp"
#include "calculation_engine/transport/market_data_decoder.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace ce = calculation_engine;

namespace {

using Bytes = std::vector<std::uint8_t>;

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Integer>
void append_big_endian(Bytes& output, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t shift = sizeof(Integer); shift > 0; --shift) {
        output.push_back(static_cast<std::uint8_t>(
            value >> ((shift - 1U) * 8U)));
    }
}

void append_map(Bytes& output, std::uint16_t size) {
    output.push_back(0xde);
    append_big_endian(output, size);
}

void append_array(Bytes& output, std::uint16_t size) {
    output.push_back(0xdc);
    append_big_endian(output, size);
}

void append_string(Bytes& output, const std::string& value) {
    check(
        value.size() <= std::numeric_limits<std::uint16_t>::max(),
        "test string is too large");
    output.push_back(0xda);
    append_big_endian(output, static_cast<std::uint16_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void append_unsigned(Bytes& output, std::uint64_t value) {
    output.push_back(0xcf);
    append_big_endian(output, value);
}

void append_signed(Bytes& output, std::int64_t value) {
    output.push_back(0xd3);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_big_endian(output, bits);
}

void append_bool(Bytes& output, bool value) {
    output.push_back(value ? 0xc3 : 0xc2);
}

void append_nil(Bytes& output) {
    output.push_back(0xc0);
}

void append_double(Bytes& output, double value) {
    output.push_back(0xcb);
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_big_endian(output, bits);
}

void append_key(Bytes& output, const std::string& key) {
    append_string(output, key);
}

Bytes order(
    std::uint64_t entry_id,
    const std::string& symbol,
    const std::string& side,
    std::int64_t price,
    std::int64_t quantity,
    bool is_market_maker = false) {
    Bytes output;
    append_map(output, 11);
    append_key(output, "entry_id");
    append_unsigned(output, entry_id);
    append_key(output, "order_id");
    append_unsigned(output, entry_id);
    append_key(output, "agent_id");
    append_unsigned(output, 42);
    append_key(output, "priority_time_ns");
    append_signed(output, 100);
    append_key(output, "symbol");
    append_string(output, symbol);
    append_key(output, "side");
    append_string(output, side);
    append_key(output, "price");
    append_signed(output, price);
    append_key(output, "quantity");
    append_signed(output, quantity);
    append_key(output, "visibility");
    append_string(output, "VISIBLE");
    append_key(output, "insert_by_id");
    append_bool(output, false);
    append_key(output, "is_market_maker");
    append_bool(output, is_market_maker);
    return output;
}

Bytes snapshot_payload(const std::vector<Bytes>& orders) {
    Bytes output;
    append_map(output, 2);
    append_key(output, "orders");
    append_array(output, static_cast<std::uint16_t>(orders.size()));
    for (const auto& item : orders) {
        output.insert(output.end(), item.begin(), item.end());
    }
    append_key(output, "last_trade_price");
    append_nil(output);
    return output;
}

Bytes add_payload(const Bytes& image) {
    Bytes output;
    append_map(output, 1);
    append_key(output, "order");
    output.insert(output.end(), image.begin(), image.end());
    return output;
}

Bytes execute_payload() {
    Bytes output;
    append_map(output, 6);
    append_key(output, "passive_entry_id");
    append_unsigned(output, 2);
    append_key(output, "aggressor_entry_id");
    append_nil(output);
    append_key(output, "execution_price");
    append_signed(output, 102);
    append_key(output, "executed_quantity");
    append_signed(output, 5);
    append_key(output, "passive_remaining_quantity");
    append_signed(output, 15);
    append_key(output, "aggressor_side");
    append_string(output, "BID");
    return output;
}

Bytes envelope(
    const std::string& event_type,
    std::uint64_t transport_sequence,
    std::uint64_t source_sequence,
    const std::string& symbol,
    const Bytes& payload) {
    Bytes message;
    append_map(message, 3);
    append_key(message, "protocol_version");
    append_unsigned(message, 1);
    append_key(message, "kind");
    append_string(message, "MARKET_DATA");
    append_key(message, "body");
    append_map(message, 11);
    append_key(message, "schema_version");
    append_unsigned(message, 1);
    append_key(message, "run_id");
    append_string(message, "run-1");
    append_key(message, "transport_sequence");
    append_unsigned(message, transport_sequence);
    append_key(message, "channel_id");
    append_string(message, symbol.empty() ? "SYSTEM" : symbol);
    append_key(message, "source_sequence");
    append_unsigned(message, source_sequence);
    append_key(message, "sim_time_ns");
    append_signed(message, 1'000);
    append_key(message, "generated_wall_time_ns");
    append_unsigned(message, 2'000);
    append_key(message, "event_type");
    append_string(message, event_type);
    append_key(message, "symbol");
    append_string(message, symbol);
    append_key(message, "payload");
    message.insert(message.end(), payload.begin(), payload.end());
    append_key(message, "playback_speed");
    append_double(message, 1.0);

    Bytes frame{'M', 'D', 'P', '1', 0};
    frame.insert(frame.end(), message.begin(), message.end());
    return frame;
}

Bytes compress_frame(const Bytes& frame) {
    check(frame.size() >= 5, "test frame is too short");
    const auto payload_size = frame.size() - 5U;
    uLongf compressed_size = compressBound(static_cast<uLong>(payload_size));
    Bytes compressed(static_cast<std::size_t>(compressed_size));
    const auto result = compress2(
        compressed.data(),
        &compressed_size,
        frame.data() + 5U,
        static_cast<uLong>(payload_size),
        3);
    check(result == Z_OK, "unable to compress test frame");
    compressed.resize(static_cast<std::size_t>(compressed_size));

    Bytes output{'M', 'D', 'P', '1', 1};
    output.insert(output.end(), compressed.begin(), compressed.end());
    return output;
}

void test_snapshot_add_execute_pipeline() {
    ce::engine::CalculationEngine engine;

    auto frame = envelope(
        "SNAPSHOT",
        1,
        10,
        "TOPIX",
        snapshot_payload({}));
    auto decoded = ce::transport::decode_market_data_frame(frame, 3'000);
    check(decoded.run_id == "run-1", "run_id was not decoded");
    check(decoded.transport_sequence == 1, "transport sequence was not decoded");
    check(decoded.is_market_event(), "snapshot was treated as lifecycle data");
    check(
        std::holds_alternative<ce::market_data::SnapshotEvent>(
            decoded.market_event->payload),
        "snapshot payload has the wrong variant");
    check(engine.apply(*decoded.market_event).ok(), "snapshot was rejected");

    frame = envelope(
        "ADD",
        2,
        11,
        "TOPIX",
        add_payload(order(1, "TOPIX", "BID", 100, 10, true)));
    decoded = ce::transport::decode_market_data_frame(frame, 3'001);
    const auto& bid_add =
        std::get<ce::market_data::AddEvent>(decoded.market_event->payload);
    check(
        bid_add.order.is_market_maker,
        "market-maker flag was decoded incorrectly");
    check(engine.apply(*decoded.market_event).ok(), "bid add was rejected");

    frame = envelope(
        "ADD",
        3,
        12,
        "TOPIX",
        add_payload(order(2, "TOPIX", "ASK", 102, 20)));
    decoded = ce::transport::decode_market_data_frame(frame, 3'002);
    const auto add_result = engine.apply(*decoded.market_event);
    check(add_result.ok(), "ask add was rejected");
    check(
        add_result.metrics->quoted_spread == 2,
        "decoded adds produced the wrong spread");

    frame = compress_frame(envelope(
        "EXECUTE",
        4,
        13,
        "TOPIX",
        execute_payload()));
    decoded = ce::transport::decode_market_data_frame(frame, 3'003);
    const auto execution_result = engine.apply(*decoded.market_event);
    check(execution_result.ok(), "compressed execution was rejected");
    check(
        execution_result.metrics->traded_volume == 5,
        "execution volume was decoded incorrectly");
    check(
        execution_result.metrics->best_ask_quantity == 15,
        "remaining quantity was decoded incorrectly");
}

void test_all_event_variants_decode() {
    struct Case {
        std::string type;
        Bytes payload;
        std::size_t variant_index;
    };

    Bytes delete_payload;
    append_map(delete_payload, 1);
    append_key(delete_payload, "entry_id");
    append_unsigned(delete_payload, 1);

    Bytes partial_cancel_payload;
    append_map(partial_cancel_payload, 3);
    append_key(partial_cancel_payload, "entry_id");
    append_unsigned(partial_cancel_payload, 1);
    append_key(partial_cancel_payload, "cancelled_quantity");
    append_signed(partial_cancel_payload, 2);
    append_key(partial_cancel_payload, "remaining_quantity");
    append_signed(partial_cancel_payload, 8);

    Bytes modify_payload;
    append_map(modify_payload, 2);
    append_key(modify_payload, "entry_id");
    append_unsigned(modify_payload, 1);
    append_key(modify_payload, "new_quantity");
    append_signed(modify_payload, 9);

    Bytes replace_payload;
    append_map(replace_payload, 2);
    append_key(replace_payload, "old_entry_id");
    append_unsigned(replace_payload, 1);
    append_key(replace_payload, "replacement");
    const auto replacement = order(3, "TOPIX", "ASK", 103, 4);
    replace_payload.insert(
        replace_payload.end(),
        replacement.begin(),
        replacement.end());

    const std::vector<Case> cases{
        {"ADD", add_payload(order(1, "TOPIX", "BID", 100, 10)), 0},
        {"DELETE", delete_payload, 1},
        {"PARTIAL_CANCEL", partial_cancel_payload, 2},
        {"MODIFY", modify_payload, 3},
        {"REPLACE", replace_payload, 4},
        {"EXECUTE", execute_payload(), 5},
        {"SNAPSHOT", snapshot_payload({}), 6},
    };

    std::uint64_t transport_sequence = 1;
    for (const auto& item : cases) {
        const auto decoded = ce::transport::decode_market_data_frame(
            envelope(
                item.type,
                transport_sequence,
                transport_sequence,
                "TOPIX",
                item.payload),
            4'000);
        check(decoded.is_market_event(), item.type + " was not a market event");
        check(
            decoded.market_event->payload.index() == item.variant_index,
            item.type + " decoded to the wrong event variant");
        ++transport_sequence;
    }
}

void test_lifecycle_and_validation() {
    Bytes empty_payload;
    append_map(empty_payload, 0);
    const auto lifecycle = ce::transport::decode_market_data_frame(
        envelope("HEARTBEAT", 1, 0, "", empty_payload),
        5'000);
    check(!lifecycle.is_market_event(), "lifecycle event entered the engine");

    auto invalid = envelope("HEARTBEAT", 1, 0, "", empty_payload);
    invalid[0] = 'X';
    try {
        (void)ce::transport::decode_market_data_frame(invalid, 5'000);
        throw std::runtime_error("invalid header was accepted");
    } catch (const ce::transport::WireDecodeError&) {
    }

    const auto compressed = compress_frame(
        envelope("HEARTBEAT", 2, 0, "", empty_payload));
    try {
        (void)ce::transport::decode_market_data_frame(
            compressed,
            5'001,
            8);
        throw std::runtime_error("decompression limit was not enforced");
    } catch (const ce::transport::WireDecodeError&) {
    }
}

}  // namespace

int main() {
    try {
        test_snapshot_add_execute_pipeline();
        test_all_event_variants_decode();
        test_lifecycle_and_validation();
        std::cout << "market-data decoder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
