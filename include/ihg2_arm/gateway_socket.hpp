#pragma once

#include "ihg2_arm/gateway_frame.hpp"

#include <array>
#include <cstdint>

namespace ihg2::arm {

enum class ReceiveStatus : std::uint8_t {
    datagram,
    timeout,
    malformed,
    socket_error,
};

// One connected UDP socket pins the gateway peer and the ephemeral local port.
// The returned bytes remain valid until the next receive call. Packet decoding
// is deliberately a separate step and must happen before state is changed.
class GatewaySocket final {
public:
    GatewaySocket() noexcept = default;
    ~GatewaySocket();

    GatewaySocket(const GatewaySocket&) = delete;
    GatewaySocket& operator=(const GatewaySocket&) = delete;

    [[nodiscard]] bool open(const char* gateway_ipv4, std::uint16_t gateway_port) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] int error_number() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;

    [[nodiscard]] bool send_packet(const std::uint8_t* bytes, std::size_t size) noexcept;
    [[nodiscard]] ReceiveStatus receive(std::uint32_t timeout_ms, GatewayDatagramView& datagram) noexcept;

private:
    int fd_{-1};
    int error_number_{0};
    std::array<std::uint8_t, kGatewayDatagramLimit> buffer_{};
};

} // namespace ihg2::arm
