#pragma once

#include "calculation_engine/market_data/market_event.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace calculation_engine::transport {

class WireDecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct DecodedEnvelope {
    std::string run_id;
    std::uint64_t transport_sequence{};
    std::string channel_id;
    std::string event_type;
    market_data::TimestampNs sim_time_ns{};
    std::optional<market_data::RoutedMarketEvent> market_event;

    [[nodiscard]] bool is_market_event() const noexcept {
        return market_event.has_value();
    }
};

// Decodes the canonical MDP1 frame emitted by market-data-protocol. Both plain
// and zlib-compressed MessagePack payloads are accepted. Lifecycle records are
// returned without a RoutedMarketEvent so callers can observe them without
// passing them to the calculation engine.
[[nodiscard]] DecodedEnvelope decode_market_data_frame(
    const std::uint8_t* frame,
    std::size_t frame_size,
    market_data::TimestampNs received_wall_time_ns,
    std::size_t maximum_uncompressed_bytes = 64U * 1024U * 1024U);

[[nodiscard]] inline DecodedEnvelope decode_market_data_frame(
    const std::vector<std::uint8_t>& frame,
    market_data::TimestampNs received_wall_time_ns,
    std::size_t maximum_uncompressed_bytes = 64U * 1024U * 1024U) {
    return decode_market_data_frame(
        frame.data(),
        frame.size(),
        received_wall_time_ns,
        maximum_uncompressed_bytes);
}

}  // namespace calculation_engine::transport
