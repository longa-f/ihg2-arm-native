#include "ihg2_arm/vendor_protocol/protocol.hpp"
#include "ihg2_arm/gateway_socket.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <netinet/in.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace ihg2::arm;

namespace {

Bytes unhex(const char* text) {
    Bytes result;
    for (std::size_t i = 0; text[i] != '\0'; i += 2) {
        const auto byte = static_cast<std::uint8_t>(std::stoul(std::string(text + i, 2), nullptr, 16));
        result.push_back(byte);
    }
    return result;
}

template <typename Function>
void rejects(Function&& function) {
    bool rejected = false;
    try {
        function();
    } catch (const ProtocolError&) {
        rejected = true;
    }
    assert(rejected);
}

} // namespace

int main() {
    const auto key = parse_key("000102030405060708090a0b0c0d0e0f");
    const Identity identity{parse_mac("01:02:03:04:05:06"), 1, 0x274a};

    // Golden vectors are the new C++ bridge's offline protocol fixtures; the key is test-only.
    const auto expected_heartbeat = unhex(
            "5aa5aa555aa5aa55000000000000000000000000000000000000000000000000"
            "2fcd00004a27690024810605040302010100000036c20000"
            "bb38d8b335348cd5ae486dca908c2046");
    const auto heartbeat = encode_packet(nullptr, key, identity, 0x0069, 0x8124, 0x0b02, 1);
    assert(heartbeat == expected_heartbeat);

    const auto expected_response = unhex(
            "5aa5aa555aa5aa55000000000000000000000000000000000000000000000000"
            "79e000004a27ee032381060504030201010000002ec90000"
            "e53ed4d8b86758acb412e2893f28809b474270e5a27558109833e8fbb34bf6011bfc98f3fec81835eb4cf4afd3f3b60b");
    const auto response = decode_packet(expected_response, key, identity, true, true);
    assert(response.command == 0x03ee);
    assert(response.count == 0x8123);
    assert(response.payload == Json({{"did", "panel"}, {"pwr1", 1}}));

    // Installed firmware acknowledges the heartbeat with an empty 0x0000 inner type.
    const auto ack_packet = encode_packet(nullptr, key, identity, response_heartbeat, 0x8125, 0x0000, 1);
    const auto ack = decode_packet(ack_packet, key, identity, true, true);
    assert(ack.command == response_heartbeat && ack.message_type == 0 && ack.payload.is_null());

    auto damaged = expected_response;
    damaged.back() ^= 1;
    rejects([&] { (void)decode_packet(damaged, key, identity); });
    rejects([&] { (void)decode_packet(expected_response, key, Identity{}, true); });

    const auto wrong_type = encode_packet(nullptr, key, identity, response_get, 0x8126, 0x0000, 1);
    rejects([&] { (void)decode_packet(wrong_type, key, identity); });
    rejects([] { (void)parse_key("bad-key"); });
    rejects([] { (void)parse_mac("bad-mac"); });
    rejects([] { (void)bounded_json(std::string(8193, ' ')); });

    // Exercise the same connected UDP socket for heartbeat TX and pushed state RX.
    const int mock_gateway = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(mock_gateway >= 0);
    sockaddr_in gateway_address{};
    gateway_address.sin_family = AF_INET;
    gateway_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    gateway_address.sin_port = htons(0);
    assert(::bind(mock_gateway,
                  reinterpret_cast<const sockaddr*>(&gateway_address),
                  sizeof gateway_address) == 0);
    socklen_t gateway_address_size = sizeof gateway_address;
    assert(::getsockname(mock_gateway,
                         reinterpret_cast<sockaddr*>(&gateway_address),
                         &gateway_address_size) == 0);

    GatewaySocket socket;
    assert(socket.open("127.0.0.1", ntohs(gateway_address.sin_port)));
    assert(socket.send_packet(heartbeat.data(), heartbeat.size()));
    std::array<std::uint8_t, kGatewayDatagramLimit> received_heartbeat{};
    sockaddr_in heartbeat_sender{};
    socklen_t heartbeat_sender_size = sizeof heartbeat_sender;
    const auto received_heartbeat_size = ::recvfrom(
            mock_gateway,
            received_heartbeat.data(),
            received_heartbeat.size(),
            0,
            reinterpret_cast<sockaddr*>(&heartbeat_sender),
            &heartbeat_sender_size);
    assert(received_heartbeat_size == static_cast<ssize_t>(heartbeat.size()));
    assert(std::equal(heartbeat.begin(), heartbeat.end(), received_heartbeat.begin()));
    assert(ntohs(heartbeat_sender.sin_port) == socket.local_port());

    sockaddr_in bridge_address{};
    bridge_address.sin_family = AF_INET;
    bridge_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bridge_address.sin_port = htons(socket.local_port());
    assert(::sendto(mock_gateway,
                    expected_response.data(),
                    expected_response.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&bridge_address),
                    sizeof bridge_address) == static_cast<ssize_t>(expected_response.size()));
    GatewayDatagramView datagram{};
    assert(socket.receive(1000, datagram) == ReceiveStatus::datagram);
    const auto received_frame = decode_packet(
            std::span<const std::uint8_t>(datagram.bytes, datagram.size), key, identity, true, true);
    assert(received_frame.payload == response.payload);
    socket.close();
    ::close(mock_gateway);

    return 0;
}
