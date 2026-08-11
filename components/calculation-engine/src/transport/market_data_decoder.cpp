#include "calculation_engine/transport/market_data_decoder.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace calculation_engine::transport {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'M', 'D', 'P', '1'};
constexpr std::uint8_t kCompressedFlag = 0x01;
constexpr std::uint8_t kKnownFlags = kCompressedFlag;
constexpr std::uint64_t kProtocolVersion = 1;
constexpr std::uint64_t kSchemaVersion = 1;
constexpr std::size_t kMaximumNestingDepth = 64;
constexpr std::size_t kMaximumContainerEntries = 1'000'000;

struct Nil {};
using Binary = std::vector<std::uint8_t>;

struct Value {
    using Array = std::vector<Value>;
    using Object = std::unordered_map<std::string, std::shared_ptr<Value>>;
    using Storage = std::variant<
        Nil,
        bool,
        std::int64_t,
        std::uint64_t,
        double,
        std::string,
        Binary,
        Array,
        Object>;

    Storage storage;
};

[[noreturn]] void fail(const std::string& message) {
    throw WireDecodeError(message);
}

void validate_utf8(std::string_view text, const std::string& field) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[offset]);
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7fU) {
            ++offset;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            code_point = first & 0x1fU;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            code_point = first & 0x0fU;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            fail(field + " contains invalid UTF-8");
        }
        if (offset + continuation_count >= text.size()) {
            fail(field + " contains truncated UTF-8");
        }
        for (std::size_t index = 1; index <= continuation_count; ++index) {
            const auto next = static_cast<std::uint8_t>(text[offset + index]);
            if ((next & 0xc0U) != 0x80U) {
                fail(field + " contains invalid UTF-8");
            }
            code_point = (code_point << 6U) | (next & 0x3fU);
        }
        const bool overlong =
            (continuation_count == 1 && code_point < 0x80U) ||
            (continuation_count == 2 && code_point < 0x800U) ||
            (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || (code_point >= 0xd800U && code_point <= 0xdfffU) ||
            code_point > 0x10ffffU) {
            fail(field + " contains invalid UTF-8");
        }
        offset += continuation_count + 1;
    }
}

class MessagePackParser {
public:
    MessagePackParser(const std::uint8_t* data, std::size_t size)
        : current_(data), end_(data + size) {}

    [[nodiscard]] Value parse_document() {
        auto result = parse_value(0);
        if (current_ != end_) {
            fail("MessagePack payload contains trailing bytes");
        }
        return result;
    }

private:
    [[nodiscard]] std::uint8_t read_byte() {
        if (current_ == end_) {
            fail("truncated MessagePack payload");
        }
        return *current_++;
    }

    template <typename Integer>
    [[nodiscard]] Integer read_big_endian() {
        static_assert(std::is_unsigned_v<Integer>);
        if (static_cast<std::size_t>(end_ - current_) < sizeof(Integer)) {
            fail("truncated MessagePack integer");
        }
        Integer value = 0;
        for (std::size_t index = 0; index < sizeof(Integer); ++index) {
            value = static_cast<Integer>(
                (value << 8U) | static_cast<Integer>(*current_++));
        }
        return value;
    }

    [[nodiscard]] std::size_t checked_size(std::uint64_t value, const char* kind) {
        if (value > kMaximumContainerEntries) {
            fail(std::string("MessagePack ") + kind + " exceeds safety limit");
        }
        return static_cast<std::size_t>(value);
    }

    [[nodiscard]] std::string read_string(std::size_t size) {
        if (static_cast<std::size_t>(end_ - current_) < size) {
            fail("truncated MessagePack string");
        }
        std::string value(
            reinterpret_cast<const char*>(current_),
            size);
        current_ += size;
        validate_utf8(value, "MessagePack string");
        return value;
    }

    [[nodiscard]] Binary read_binary(std::size_t size) {
        if (static_cast<std::size_t>(end_ - current_) < size) {
            fail("truncated MessagePack binary value");
        }
        Binary value(current_, current_ + size);
        current_ += size;
        return value;
    }

    [[nodiscard]] Value parse_array(std::size_t count, std::size_t depth) {
        if (count > kMaximumContainerEntries) {
            fail("MessagePack array exceeds safety limit");
        }
        Value::Array values;
        values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            values.push_back(parse_value(depth + 1));
        }
        return Value{std::move(values)};
    }

    [[nodiscard]] Value parse_object(std::size_t count, std::size_t depth) {
        if (count > kMaximumContainerEntries) {
            fail("MessagePack map exceeds safety limit");
        }
        Value::Object values;
        values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto key = parse_value(depth + 1);
            const auto* key_text = std::get_if<std::string>(&key.storage);
            if (key_text == nullptr) {
                fail("MessagePack map keys must be strings");
            }
            auto inserted = values.emplace(*key_text, nullptr);
            if (!inserted.second) {
                fail("MessagePack map contains duplicate key: " + *key_text);
            }
            inserted.first->second =
                std::make_shared<Value>(parse_value(depth + 1));
        }
        return Value{std::move(values)};
    }

    [[nodiscard]] Value parse_value(std::size_t depth) {
        if (depth > kMaximumNestingDepth) {
            fail("MessagePack nesting exceeds safety limit");
        }

        const auto marker = read_byte();
        if (marker <= 0x7fU) {
            return Value{static_cast<std::uint64_t>(marker)};
        }
        if (marker >= 0xe0U) {
            return Value{static_cast<std::int64_t>(static_cast<std::int8_t>(marker))};
        }
        if ((marker & 0xe0U) == 0xa0U) {
            return Value{read_string(static_cast<std::size_t>(marker & 0x1fU))};
        }
        if ((marker & 0xf0U) == 0x90U) {
            return parse_array(static_cast<std::size_t>(marker & 0x0fU), depth);
        }
        if ((marker & 0xf0U) == 0x80U) {
            return parse_object(static_cast<std::size_t>(marker & 0x0fU), depth);
        }

        switch (marker) {
            case 0xc0:
                return Value{Nil{}};
            case 0xc2:
                return Value{false};
            case 0xc3:
                return Value{true};
            case 0xc4:
                return Value{read_binary(read_big_endian<std::uint8_t>())};
            case 0xc5:
                return Value{read_binary(read_big_endian<std::uint16_t>())};
            case 0xc6:
                return Value{read_binary(
                    static_cast<std::size_t>(read_big_endian<std::uint32_t>()))};
            case 0xca: {
                const auto bits = read_big_endian<std::uint32_t>();
                float value = 0.0F;
                std::memcpy(&value, &bits, sizeof(value));
                return Value{static_cast<double>(value)};
            }
            case 0xcb: {
                const auto bits = read_big_endian<std::uint64_t>();
                double value = 0.0;
                std::memcpy(&value, &bits, sizeof(value));
                return Value{value};
            }
            case 0xcc:
                return Value{static_cast<std::uint64_t>(
                    read_big_endian<std::uint8_t>())};
            case 0xcd:
                return Value{static_cast<std::uint64_t>(
                    read_big_endian<std::uint16_t>())};
            case 0xce:
                return Value{static_cast<std::uint64_t>(
                    read_big_endian<std::uint32_t>())};
            case 0xcf:
                return Value{read_big_endian<std::uint64_t>()};
            case 0xd0:
                return Value{static_cast<std::int64_t>(
                    static_cast<std::int8_t>(read_big_endian<std::uint8_t>()))};
            case 0xd1:
                return Value{static_cast<std::int64_t>(
                    static_cast<std::int16_t>(read_big_endian<std::uint16_t>()))};
            case 0xd2:
                return Value{static_cast<std::int64_t>(
                    static_cast<std::int32_t>(read_big_endian<std::uint32_t>()))};
            case 0xd3: {
                const auto bits = read_big_endian<std::uint64_t>();
                std::int64_t value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                return Value{value};
            }
            case 0xd9:
                return Value{read_string(read_big_endian<std::uint8_t>())};
            case 0xda:
                return Value{read_string(read_big_endian<std::uint16_t>())};
            case 0xdb:
                return Value{read_string(
                    static_cast<std::size_t>(read_big_endian<std::uint32_t>()))};
            case 0xdc:
                return parse_array(
                    checked_size(read_big_endian<std::uint16_t>(), "array"),
                    depth);
            case 0xdd:
                return parse_array(
                    checked_size(read_big_endian<std::uint32_t>(), "array"),
                    depth);
            case 0xde:
                return parse_object(
                    checked_size(read_big_endian<std::uint16_t>(), "map"),
                    depth);
            case 0xdf:
                return parse_object(
                    checked_size(read_big_endian<std::uint32_t>(), "map"),
                    depth);
            default:
                fail("unsupported MessagePack marker");
        }
    }

    const std::uint8_t* current_;
    const std::uint8_t* end_;
};

[[nodiscard]] const Value::Object& require_object(
    const Value& value,
    const std::string& field) {
    const auto* result = std::get_if<Value::Object>(&value.storage);
    if (result == nullptr) {
        fail(field + " must be a mapping");
    }
    return *result;
}

[[nodiscard]] const Value& require_field(
    const Value::Object& object,
    const std::string& field) {
    const auto found = object.find(field);
    if (found == object.end()) {
        fail("missing field: " + field);
    }
    return *found->second;
}

[[nodiscard]] const Value* optional_field(
    const Value::Object& object,
    const std::string& field) {
    const auto found = object.find(field);
    return found == object.end() ? nullptr : found->second.get();
}

[[nodiscard]] bool is_nil(const Value& value) {
    return std::holds_alternative<Nil>(value.storage);
}

[[nodiscard]] std::string require_string(
    const Value& value,
    const std::string& field,
    bool allow_empty = false) {
    const auto* result = std::get_if<std::string>(&value.storage);
    if (result == nullptr) {
        fail(field + " must be a string");
    }
    if (!allow_empty && result->empty()) {
        fail(field + " must not be empty");
    }
    return *result;
}

[[nodiscard]] bool require_bool(const Value& value, const std::string& field) {
    const auto* result = std::get_if<bool>(&value.storage);
    if (result == nullptr) {
        fail(field + " must be a bool");
    }
    return *result;
}

[[nodiscard]] std::uint64_t require_unsigned(
    const Value& value,
    const std::string& field,
    std::uint64_t minimum = 0) {
    std::uint64_t result = 0;
    if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value.storage)) {
        result = *unsigned_value;
    } else if (const auto* signed_value = std::get_if<std::int64_t>(&value.storage);
               signed_value != nullptr && *signed_value >= 0) {
        result = static_cast<std::uint64_t>(*signed_value);
    } else {
        fail(field + " must be a non-negative integer");
    }
    if (result < minimum) {
        fail(field + " is below its minimum");
    }
    return result;
}

[[nodiscard]] std::int64_t require_signed(
    const Value& value,
    const std::string& field,
    std::optional<std::int64_t> minimum = std::nullopt) {
    std::int64_t result = 0;
    if (const auto* signed_value = std::get_if<std::int64_t>(&value.storage)) {
        result = *signed_value;
    } else if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value.storage);
               unsigned_value != nullptr &&
               *unsigned_value <=
                   static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result = static_cast<std::int64_t>(*unsigned_value);
    } else {
        fail(field + " must be an integer in the signed 64-bit range");
    }
    if (minimum.has_value() && result < *minimum) {
        fail(field + " is below its minimum");
    }
    return result;
}

template <typename Integer>
[[nodiscard]] Integer require_unsigned_as(
    const Value& value,
    const std::string& field,
    std::uint64_t minimum = 0) {
    const auto result = require_unsigned(value, field, minimum);
    if (result > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
        fail(field + " is outside the target integer range");
    }
    return static_cast<Integer>(result);
}

[[nodiscard]] market_data::Side parse_side(
    const Value& value,
    const std::string& field) {
    const auto side = require_string(value, field);
    if (side == "BID") {
        return market_data::Side::Bid;
    }
    if (side == "ASK") {
        return market_data::Side::Ask;
    }
    fail(field + " must be BID or ASK");
}

[[nodiscard]] market_data::Visibility parse_visibility(const Value& value) {
    const auto visibility = require_string(value, "visibility");
    if (visibility == "VISIBLE") {
        return market_data::Visibility::Visible;
    }
    if (visibility == "HIDDEN") {
        return market_data::Visibility::Hidden;
    }
    fail("visibility must be VISIBLE or HIDDEN");
}

[[nodiscard]] market_data::OrderImage parse_order(const Value& value) {
    const auto& order = require_object(value, "order");
    return market_data::OrderImage{
        require_unsigned_as<market_data::EntryId>(
            require_field(order, "entry_id"), "entry_id"),
        require_unsigned_as<market_data::OrderId>(
            require_field(order, "order_id"), "order_id"),
        require_unsigned_as<market_data::AgentId>(
            require_field(order, "agent_id"), "agent_id"),
        require_signed(
            require_field(order, "priority_time_ns"), "priority_time_ns"),
        require_string(require_field(order, "symbol"), "symbol"),
        parse_side(require_field(order, "side"), "side"),
        require_signed(require_field(order, "price"), "price"),
        require_signed(
            require_field(order, "quantity"), "quantity", std::int64_t{1}),
        parse_visibility(require_field(order, "visibility")),
        require_bool(require_field(order, "insert_by_id"), "insert_by_id"),
        require_bool(require_field(order, "is_market_maker"),"is_market_maker"),
    };
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_optional_signed(
    const Value::Object& object,
    const std::string& field,
    std::optional<std::int64_t> minimum = std::nullopt) {
    const auto* value = optional_field(object, field);
    if (value == nullptr || is_nil(*value)) {
        return std::nullopt;
    }
    return static_cast<Integer>(require_signed(*value, field, minimum));
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_optional_unsigned(
    const Value::Object& object,
    const std::string& field) {
    const auto* value = optional_field(object, field);
    if (value == nullptr || is_nil(*value)) {
        return std::nullopt;
    }
    return require_unsigned_as<Integer>(*value, field);
}

[[nodiscard]] bool is_lifecycle_event(const std::string& event_type) {
    return event_type == "SIMULATION_START" ||
        event_type == "SESSION_OPEN" ||
        event_type == "HEARTBEAT" ||
        event_type == "SESSION_CLOSE" ||
        event_type == "SIMULATION_END" ||
        event_type == "ERROR";
}

[[nodiscard]] market_data::MarketEvent parse_market_event(
    const std::string& event_type,
    market_data::Sequence sequence,
    market_data::TimestampNs sim_time_ns,
    const std::string& envelope_symbol,
    const Value::Object& payload) {
    if (event_type == "ADD") {
        return market_data::AddEvent{
            sequence,
            sim_time_ns,
            parse_order(require_field(payload, "order"))};
    }
    if (event_type == "DELETE") {
        return market_data::DeleteEvent{
            sequence,
            sim_time_ns,
            require_unsigned_as<market_data::EntryId>(
                require_field(payload, "entry_id"), "entry_id")};
    }
    if (event_type == "PARTIAL_CANCEL") {
        return market_data::PartialCancelEvent{
            sequence,
            sim_time_ns,
            require_unsigned_as<market_data::EntryId>(
                require_field(payload, "entry_id"), "entry_id"),
            require_signed(
                require_field(payload, "cancelled_quantity"),
                "cancelled_quantity",
                std::int64_t{1}),
            parse_optional_signed<market_data::Quantity>(
                payload,
                "remaining_quantity",
                std::int64_t{0})};
    }
    if (event_type == "MODIFY") {
        return market_data::ModifyEvent{
            sequence,
            sim_time_ns,
            require_unsigned_as<market_data::EntryId>(
                require_field(payload, "entry_id"), "entry_id"),
            require_signed(
                require_field(payload, "new_quantity"),
                "new_quantity",
                std::int64_t{1})};
    }
    if (event_type == "REPLACE") {
        return market_data::ReplaceEvent{
            sequence,
            sim_time_ns,
            require_unsigned_as<market_data::EntryId>(
                require_field(payload, "old_entry_id"), "old_entry_id"),
            parse_order(require_field(payload, "replacement"))};
    }
    if (event_type == "EXECUTE") {
        return market_data::ExecuteEvent{
            sequence,
            sim_time_ns,
            require_unsigned_as<market_data::EntryId>(
                require_field(payload, "passive_entry_id"),
                "passive_entry_id"),
            parse_optional_unsigned<market_data::EntryId>(
                payload,
                "aggressor_entry_id"),
            require_signed(
                require_field(payload, "execution_price"),
                "execution_price"),
            require_signed(
                require_field(payload, "executed_quantity"),
                "executed_quantity",
                std::int64_t{1}),
            parse_optional_signed<market_data::Quantity>(
                payload,
                "passive_remaining_quantity",
                std::int64_t{0}),
            parse_side(
                require_field(payload, "aggressor_side"),
                "aggressor_side")};
    }
    if (event_type == "SNAPSHOT") {
        const auto& order_values = std::get_if<Value::Array>(
            &require_field(payload, "orders").storage);
        if (order_values == nullptr) {
            fail("orders must be a list");
        }
        std::vector<market_data::OrderImage> orders;
        orders.reserve(order_values->size());
        for (const auto& order : *order_values) {
            orders.push_back(parse_order(order));
        }
        return market_data::SnapshotEvent{
            sequence,
            sim_time_ns,
            envelope_symbol,
            std::move(orders),
            parse_optional_signed<market_data::Price>(
                payload,
                "last_trade_price")};
    }
    fail("unsupported event_type: " + event_type);
}

[[nodiscard]] std::vector<std::uint8_t> decompress(
    const std::uint8_t* data,
    std::size_t size,
    std::size_t maximum_uncompressed_bytes) {
    if (size > static_cast<std::size_t>(std::numeric_limits<uInt>::max())) {
        fail("compressed payload is too large for zlib");
    }

    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data));
    stream.avail_in = static_cast<uInt>(size);
    if (inflateInit(&stream) != Z_OK) {
        fail("unable to initialize zlib");
    }

    std::vector<std::uint8_t> output;
    output.reserve(std::min(maximum_uncompressed_bytes, size * 4U));
    std::array<std::uint8_t, 16U * 1024U> chunk{};
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<uInt>(chunk.size());
        result = inflate(&stream, Z_NO_FLUSH);
        const auto produced =
            chunk.size() - static_cast<std::size_t>(stream.avail_out);
        if (produced > maximum_uncompressed_bytes - output.size()) {
            inflateEnd(&stream);
            fail("uncompressed payload exceeds configured limit");
        }
        output.insert(output.end(), chunk.begin(), chunk.begin() + produced);
    }
    const auto end_result = inflateEnd(&stream);
    if (result != Z_STREAM_END || end_result != Z_OK || stream.avail_in != 0) {
        fail("invalid compressed payload");
    }
    return output;
}

}  // namespace

DecodedEnvelope decode_market_data_frame(
    const std::uint8_t* frame,
    std::size_t frame_size,
    market_data::TimestampNs received_wall_time_ns,
    std::size_t maximum_uncompressed_bytes) {
    if (frame == nullptr && frame_size != 0) {
        fail("frame pointer must not be null");
    }
    if (frame_size < kMagic.size() + 1U ||
        !std::equal(kMagic.begin(), kMagic.end(), frame)) {
        fail("invalid market-data protocol header");
    }
    if (maximum_uncompressed_bytes == 0) {
        fail("maximum uncompressed frame size must be positive");
    }

    const auto flags = frame[kMagic.size()];
    if ((flags & static_cast<std::uint8_t>(~kKnownFlags)) != 0) {
        fail("unsupported market-data protocol flags");
    }
    const auto* payload = frame + kMagic.size() + 1U;
    const auto payload_size = frame_size - kMagic.size() - 1U;

    std::vector<std::uint8_t> uncompressed;
    if ((flags & kCompressedFlag) != 0) {
        uncompressed = decompress(
            payload,
            payload_size,
            maximum_uncompressed_bytes);
        payload = uncompressed.data();
    } else if (payload_size > maximum_uncompressed_bytes) {
        fail("payload exceeds configured limit");
    }
    const auto decoded_size =
        (flags & kCompressedFlag) != 0 ? uncompressed.size() : payload_size;
    if (decoded_size == 0) {
        fail("empty MessagePack payload");
    }

    const auto document = MessagePackParser(payload, decoded_size).parse_document();
    const auto& root = require_object(document, "wire message");
    if (require_unsigned(require_field(root, "protocol_version"), "protocol_version") !=
        kProtocolVersion) {
        fail("unsupported protocol_version");
    }
    if (require_string(require_field(root, "kind"), "kind") != "MARKET_DATA") {
        fail("calculation engine expects kind=MARKET_DATA");
    }

    const auto& body = require_object(require_field(root, "body"), "body");
    const auto* schema_version = optional_field(body, "schema_version");
    if (schema_version != nullptr &&
        require_unsigned(*schema_version, "schema_version") != kSchemaVersion) {
        fail("unsupported schema_version");
    }

    DecodedEnvelope result;
    result.run_id = require_string(require_field(body, "run_id"), "run_id");
    result.transport_sequence = require_unsigned(
        require_field(body, "transport_sequence"),
        "transport_sequence",
        1);
    result.channel_id = require_string(
        require_field(body, "channel_id"),
        "channel_id");
    const auto source_sequence = require_unsigned_as<market_data::Sequence>(
        require_field(body, "source_sequence"),
        "source_sequence");
    result.sim_time_ns = require_signed(
        require_field(body, "sim_time_ns"),
        "sim_time_ns");
    (void)require_unsigned(
        require_field(body, "generated_wall_time_ns"),
        "generated_wall_time_ns");
    result.event_type = require_string(
        require_field(body, "event_type"),
        "event_type");
    const auto* symbol_value = optional_field(body, "symbol");
    const auto symbol = symbol_value == nullptr
        ? std::string{}
        : require_string(*symbol_value, "symbol", true);
    const auto* payload_value = optional_field(body, "payload");
    const Value::Object empty_payload;
    const auto& payload_object = payload_value == nullptr
        ? empty_payload
        : require_object(*payload_value, "payload");

    if (is_lifecycle_event(result.event_type)) {
        return result;
    }
    if (symbol.empty()) {
        fail("market-data events require a symbol");
    }

    result.market_event = market_data::RoutedMarketEvent{
        symbol,
        received_wall_time_ns,
        parse_market_event(
            result.event_type,
            source_sequence,
            result.sim_time_ns,
            symbol,
            payload_object)};
    return result;
}

}  // namespace calculation_engine::transport
