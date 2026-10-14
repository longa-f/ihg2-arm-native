#pragma once
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <vector>
namespace ihg2::arm {
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;
using Key = std::array<std::uint8_t, 16>;
using Mac = std::array<std::uint8_t, 6>;
constexpr std::uint16_t response_get = 0x03ee, response_heartbeat = 0x03ed;
struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Identity {
    Mac mac{};
    std::uint32_t device_id{1};
    std::uint16_t devtype{0x274a};
};
struct Frame {
    std::uint16_t command{}, count{}, error{}, message_type{}, flags{};
    Json payload;
};
Key parse_key(const std::string &);
Mac parse_mac(const std::string &);
Json bounded_json(std::string_view text, std::size_t max_bytes = 8192, std::size_t max_nodes = 2048);
std::uint16_t checksum(std::span<const std::uint8_t>);
Bytes encode_packet(const Json &payload, const Key &, const Identity &, std::uint16_t command,
                    std::uint16_t count, std::uint16_t message_type = 0x0b02, std::uint16_t flags = 0);
Frame decode_packet(std::span<const std::uint8_t>, const Key &, const Identity &, bool strict_identity = true,
                    bool verify_payload_checksum = false);
std::string fingerprint(std::span<const std::uint8_t>);
} // namespace ihg2::arm
