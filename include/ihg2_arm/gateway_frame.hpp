#pragma once

#include <cstddef>
#include <cstdint>

namespace ihg2::arm {

// A transport-only view. Contents are untrusted until decode_packet() verifies
// the IHG2 checksum, identity, ciphertext structure, and inner payload.
constexpr std::size_t kGatewayDatagramLimit = 8192;

struct GatewayDatagramView {
    const std::uint8_t* bytes{nullptr};
    std::size_t size{0};
};

} // namespace ihg2::arm
