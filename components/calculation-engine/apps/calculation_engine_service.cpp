#include "calculation_engine/engine/calculation_engine.hpp"
#include "calculation_engine/transport/market_data_decoder.hpp"

#include <zmq.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace ce = calculation_engine;

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

struct Options {
    std::string endpoint{"tcp://127.0.0.1:5558"};
    int receive_hwm{100'000};
    int linger_ms{0};
    std::size_t maximum_frame_bytes{64U * 1024U * 1024U};
    std::size_t metric_depth_levels{10};
    std::uint64_t maximum_messages{0};
};

[[noreturn]] void usage_error(const std::string& message) {
    throw std::invalid_argument(message + "; use --help for usage");
}

template <typename Integer>
Integer parse_integer(std::string_view text, const std::string& option) {
    Integer value{};
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        usage_error(option + " requires an integer");
    }
    return value;
}

void print_usage(const char* program) {
    std::cout
        << "Usage: " << program << " [options]\n"
        << "\n"
        << "Receive canonical market-data frames from the pacing server and emit\n"
        << "one JSON metric record per synchronized market event.\n"
        << "\n"
        << "Options:\n"
        << "  --endpoint ENDPOINT       ZeroMQ PULL endpoint to connect to\n"
        << "                            (default tcp://127.0.0.1:5558)\n"
        << "  --receive-hwm COUNT       ZeroMQ receive high-water mark\n"
        << "  --linger-ms MILLISECONDS  Socket close linger period\n"
        << "  --max-frame-bytes BYTES   Maximum wire and decompressed frame size\n"
        << "  --metric-depth LEVELS     Visible depth levels used for metrics\n"
        << "  --max-messages COUNT      Exit after COUNT decoded envelopes; 0 is unlimited\n"
        << "  -h, --help                Show this help\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            usage_error("missing value for " + argument);
        }
        const std::string_view value = argv[++index];
        if (argument == "--endpoint") {
            options.endpoint = value;
            if (options.endpoint.empty()) {
                usage_error("--endpoint must not be empty");
            }
        } else if (argument == "--receive-hwm") {
            options.receive_hwm = parse_integer<int>(value, argument);
            if (options.receive_hwm <= 0) {
                usage_error("--receive-hwm must be positive");
            }
        } else if (argument == "--linger-ms") {
            options.linger_ms = parse_integer<int>(value, argument);
            if (options.linger_ms < 0) {
                usage_error("--linger-ms must not be negative");
            }
        } else if (argument == "--max-frame-bytes") {
            options.maximum_frame_bytes =
                parse_integer<std::size_t>(value, argument);
            if (options.maximum_frame_bytes == 0) {
                usage_error("--max-frame-bytes must be positive");
            }
        } else if (argument == "--metric-depth") {
            options.metric_depth_levels =
                parse_integer<std::size_t>(value, argument);
            if (options.metric_depth_levels == 0) {
                usage_error("--metric-depth must be positive");
            }
        } else if (argument == "--max-messages") {
            options.maximum_messages =
                parse_integer<std::uint64_t>(value, argument);
        } else {
            usage_error("unknown option: " + argument);
        }
    }
    return options;
}

[[nodiscard]] ce::market_data::TimestampNs wall_time_ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::string json_string(std::string_view text) {
    std::ostringstream output;
    output << '"';
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    output << "\\u"
                           << std::hex
                           << std::setw(4)
                           << std::setfill('0')
                           << static_cast<unsigned int>(byte)
                           << std::dec
                           << std::setfill(' ');
                } else {
                    output << character;
                }
        }
    }
    output << '"';
    return output.str();
}

template <typename Value>
void write_optional(std::ostream& output, const std::optional<Value>& value) {
    if (value.has_value()) {
        output << *value;
    } else {
        output << "null";
    }
}

void write_metrics(
    const ce::transport::DecodedEnvelope& envelope,
    const ce::indicators::MarketMetrics& metrics,
    ce::market_data::TimestampNs received_wall_time_ns) {
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
              << '{'
              << "\"run_id\":" << json_string(envelope.run_id)
              << ",\"transport_sequence\":" << envelope.transport_sequence
              << ",\"symbol\":" << json_string(metrics.symbol)
              << ",\"sequence\":" << metrics.sequence
              << ",\"exchange_time_ns\":" << metrics.exchange_time_ns
              << ",\"received_wall_time_ns\":" << received_wall_time_ns
              << ",\"synchronized\":"
              << (metrics.synchronized ? "true" : "false")
              << ",\"order_count\":" << metrics.order_count
              << ",\"best_bid_price\":";
    write_optional(std::cout, metrics.best_bid_price);
    std::cout << ",\"best_bid_quantity\":";
    write_optional(std::cout, metrics.best_bid_quantity);
    std::cout << ",\"best_ask_price\":";
    write_optional(std::cout, metrics.best_ask_price);
    std::cout << ",\"best_ask_quantity\":";
    write_optional(std::cout, metrics.best_ask_quantity);
    std::cout << ",\"quoted_spread\":";
    write_optional(std::cout, metrics.quoted_spread);
    std::cout << ",\"midprice\":";
    write_optional(std::cout, metrics.midprice);
    std::cout << ",\"microprice\":";
    write_optional(std::cout, metrics.microprice);
    std::cout << ",\"top_of_book_imbalance\":";
    write_optional(std::cout, metrics.top_of_book_imbalance);
    std::cout << ",\"bid_visible_depth\":" << metrics.bid_visible_depth
              << ",\"ask_visible_depth\":" << metrics.ask_visible_depth
              << ",\"trade_count\":" << metrics.trade_count
              << ",\"traded_volume\":" << metrics.traded_volume
              << ",\"last_trade_price\":";
    write_optional(std::cout, metrics.last_trade_price);
    std::cout<< ",\"bid_liquidity_provision_ratio\":";
    write_optional(std::cout, metrics.bid_liquidity_provision_ratio);
    std::cout<< ",\"ask_liquidity_provision_ratio\":";
    write_optional(std::cout, metrics.ask_liquidity_provision_ratio);
    std::cout << ",\"visible_checksum\":" << metrics.visible_checksum
              << "}\n";
    std::cout.flush();
}

const char* apply_code_name(ce::order_book::ApplyCode code) {
    switch (code) {
        case ce::order_book::ApplyCode::Applied:
            return "applied";
        case ce::order_book::ApplyCode::Duplicate:
            return "duplicate";
        case ce::order_book::ApplyCode::GapDetected:
            return "gap_detected";
        case ce::order_book::ApplyCode::Desynchronized:
            return "desynchronized";
        case ce::order_book::ApplyCode::Rejected:
            return "rejected";
    }
    return "unknown";
}

[[noreturn]] void throw_zmq_error(const std::string& operation) {
    throw std::runtime_error(
        operation + " failed: " + std::string(zmq_strerror(zmq_errno())));
}

class ZmqContext {
public:
    ZmqContext() : value_(zmq_ctx_new()) {
        if (value_ == nullptr) {
            throw_zmq_error("zmq_ctx_new");
        }
    }

    ~ZmqContext() {
        if (value_ != nullptr) {
            (void)zmq_ctx_term(value_);
        }
    }

    ZmqContext(const ZmqContext&) = delete;
    ZmqContext& operator=(const ZmqContext&) = delete;

    [[nodiscard]] void* get() const noexcept { return value_; }

private:
    void* value_;
};

class ZmqSocket {
public:
    ZmqSocket(void* context, int type) : value_(zmq_socket(context, type)) {
        if (value_ == nullptr) {
            throw_zmq_error("zmq_socket");
        }
    }

    ~ZmqSocket() {
        if (value_ != nullptr) {
            (void)zmq_close(value_);
        }
    }

    ZmqSocket(const ZmqSocket&) = delete;
    ZmqSocket& operator=(const ZmqSocket&) = delete;

    [[nodiscard]] void* get() const noexcept { return value_; }

private:
    void* value_;
};

class ZmqMessage {
public:
    ZmqMessage() {
        if (zmq_msg_init(&value_) != 0) {
            throw_zmq_error("zmq_msg_init");
        }
    }

    ~ZmqMessage() {
        (void)zmq_msg_close(&value_);
    }

    ZmqMessage(const ZmqMessage&) = delete;
    ZmqMessage& operator=(const ZmqMessage&) = delete;

    [[nodiscard]] zmq_msg_t* get() noexcept { return &value_; }
    [[nodiscard]] const std::uint8_t* data() const noexcept {
        return static_cast<const std::uint8_t*>(
            zmq_msg_data(const_cast<zmq_msg_t*>(&value_)));
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return zmq_msg_size(const_cast<zmq_msg_t*>(&value_));
    }

private:
    zmq_msg_t value_{};
};

int run(const Options& options) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    ZmqContext context;
    ZmqSocket socket(context.get(), ZMQ_PULL);
    if (zmq_setsockopt(
            socket.get(),
            ZMQ_RCVHWM,
            &options.receive_hwm,
            sizeof(options.receive_hwm)) != 0) {
        throw_zmq_error("setting ZMQ_RCVHWM");
    }
    if (zmq_setsockopt(
            socket.get(),
            ZMQ_LINGER,
            &options.linger_ms,
            sizeof(options.linger_ms)) != 0) {
        throw_zmq_error("setting ZMQ_LINGER");
    }
    if (zmq_connect(socket.get(), options.endpoint.c_str()) != 0) {
        throw_zmq_error("connecting PULL socket");
    }

    std::cerr << "calculation engine connected to " << options.endpoint << '\n';

    auto engine = std::make_unique<ce::engine::CalculationEngine>(
        ce::engine::EngineConfig{
            true,
            true,
            options.metric_depth_levels});
    std::string active_run_id;
    std::uint64_t last_transport_sequence = 0;
    std::uint64_t decoded_envelopes = 0;

    while (stop_requested == 0) {
        zmq_pollitem_t item{socket.get(), 0, ZMQ_POLLIN, 0};
        const auto poll_result = zmq_poll(&item, 1, 250);
        if (poll_result < 0) {
            if (zmq_errno() == EINTR) {
                continue;
            }
            throw_zmq_error("polling PULL socket");
        }
        if (poll_result == 0 || (item.revents & ZMQ_POLLIN) == 0) {
            continue;
        }

        ZmqMessage message;
        if (zmq_msg_recv(message.get(), socket.get(), 0) < 0) {
            if (zmq_errno() == EINTR) {
                continue;
            }
            throw_zmq_error("receiving market-data frame");
        }
        if (message.size() > options.maximum_frame_bytes) {
            std::cerr << "discarding oversized market-data frame: "
                      << message.size() << " bytes\n";
            continue;
        }

        const auto received_at = wall_time_ns();
        try {
            auto envelope = ce::transport::decode_market_data_frame(
                message.data(),
                message.size(),
                received_at,
                options.maximum_frame_bytes);
            ++decoded_envelopes;

            if (active_run_id.empty()) {
                active_run_id = envelope.run_id;
            } else if (active_run_id != envelope.run_id) {
                std::cerr << "market-data run changed from "
                          << active_run_id << " to " << envelope.run_id
                          << "; resetting calculation engine state\n";
                active_run_id = envelope.run_id;
                last_transport_sequence = 0;
                engine = std::make_unique<ce::engine::CalculationEngine>(
                    ce::engine::EngineConfig{
                        true,
                        true,
                        options.metric_depth_levels});
            }

            if (envelope.transport_sequence <= last_transport_sequence) {
                std::cerr << "ignoring duplicate transport_sequence="
                          << envelope.transport_sequence << '\n';
            } else {
                if (last_transport_sequence != 0 &&
                    envelope.transport_sequence != last_transport_sequence + 1) {
                    std::cerr << "transport sequence gap: expected "
                              << (last_transport_sequence + 1) << ", got "
                              << envelope.transport_sequence << '\n';
                }
                last_transport_sequence = envelope.transport_sequence;

                if (!envelope.is_market_event()) {
                    std::cerr << "lifecycle event " << envelope.event_type
                              << " at transport_sequence="
                              << envelope.transport_sequence << '\n';
                } else {
                    const auto apply_result = engine->apply(*envelope.market_event);
                    if (!apply_result.book_result.ok()) {
                        std::cerr << "event " << envelope.event_type
                                  << " for symbol="
                                  << envelope.market_event->symbol
                                  << " was "
                                  << apply_code_name(apply_result.book_result.code)
                                  << ": "
                                  << apply_result.book_result.message
                                  << '\n';
                    } else if (
                        apply_result.metrics.has_value() &&
                        apply_result.metrics->synchronized) {
                        write_metrics(
                            envelope,
                            *apply_result.metrics,
                            received_at);
                    }
                }
            }
        } catch (const ce::transport::WireDecodeError& error) {
            std::cerr << "discarding invalid market-data frame: "
                      << error.what() << '\n';
        }

        if (options.maximum_messages != 0 &&
            decoded_envelopes >= options.maximum_messages) {
            break;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "calculation_engine_service: " << error.what() << '\n';
        return 1;
    }
}
