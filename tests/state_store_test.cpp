#include "ihg2_arm/state_store.hpp"
#include "ihg2_arm/command_queue.hpp"
#include "ihg2_arm/gateway_frame.hpp"
#include "ihg2_arm/gateway_socket.hpp"

#include <algorithm>
#include <cassert>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <string>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace ihg2::arm;

int main() {
    StateStore store({{"parent/1", "餐厅主灯", AccessoryKind::light},
                      {"parent/2", "餐厅辅灯", AccessoryKind::light}});

    assert(store.size() == 2);
    assert(!store.state(0).known);
    assert(!store.apply_power_report("unknown", true, 10));

    const auto on = store.apply_power_report("parent/1", true, 20);
    assert(on && on->index == 0 && on->on && on->revision == 1);
    assert(!store.apply_power_report("parent/1", true, 25));
    assert(store.state(0).last_report_ms == 25);
    assert(!store.state(1).known);
    assert(!store.apply_power_report("parent/1", false, 24));
    assert(store.state(0).on && store.state(0).last_report_ms == 25);

    const auto off = store.apply_power_report("parent/1", false, 30);
    assert(off && !off->on && off->revision == 2);

    bool rejected = false;
    try {
        StateStore invalid({{"duplicate", "one", AccessoryKind::light},
                            {"duplicate", "two", AccessoryKind::switch_device}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        std::vector<DeviceSpec> too_many;
        too_many.reserve(129);
        for (std::size_t i = 0; i < 129; ++i) {
            too_many.emplace_back(std::to_string(i), "device", AccessoryKind::light);
        }
        StateStore invalid(std::move(too_many));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        StateStore invalid({{"id", std::string(64, 'n'), AccessoryKind::light}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    CommandQueue<2> queue;
    PowerCommand command{};
    assert(queue.push({0, true, 1}));
    assert(queue.push({1, false, 2}));
    assert(!queue.push({0, false, 3}));
    assert(queue.pop(command) && command.sequence == 1 && command.on);
    assert(queue.push({0, false, 3}));
    assert(queue.pop(command) && command.sequence == 2 && !command.on);
    assert(queue.pop(command) && command.sequence == 3 && !command.on);
    assert(!queue.pop(command));

    GatewaySocket gateway_socket;
    GatewayDatagramView datagram{};
    assert(!gateway_socket.is_open());
    assert(gateway_socket.receive(0, datagram) == ReceiveStatus::socket_error);
    assert(gateway_socket.error_number() == EBADF);
    assert(!gateway_socket.open("999.999.999.999", 80));
    assert(gateway_socket.error_number() == EINVAL);

    const int gateway_peer = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(gateway_peer >= 0);
    sockaddr_in peer_address{};
    peer_address.sin_family = AF_INET;
    peer_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    peer_address.sin_port = htons(0);
    assert(::bind(gateway_peer, reinterpret_cast<const sockaddr*>(&peer_address), sizeof peer_address) == 0);
    socklen_t peer_address_size = sizeof peer_address;
    assert(::getsockname(gateway_peer, reinterpret_cast<sockaddr*>(&peer_address), &peer_address_size) == 0);

    GatewaySocket live_socket;
    assert(live_socket.open("127.0.0.1", ntohs(peer_address.sin_port)));
    assert(live_socket.local_port() != 0);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(live_socket.local_port());

    const int unrelated_sender = ::socket(AF_INET, SOCK_DGRAM, 0);
    assert(unrelated_sender >= 0);
    const std::array<std::uint8_t, 16> short_packet{};
    assert(::sendto(unrelated_sender,
                    short_packet.data(),
                    short_packet.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&target),
                    sizeof target) == static_cast<ssize_t>(short_packet.size()));
    assert(live_socket.receive(20, datagram) == ReceiveStatus::timeout);

    // Transport returns bytes only; cryptographic and structural validation is a separate step.
    assert(::sendto(gateway_peer,
                    short_packet.data(),
                    short_packet.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&target),
                    sizeof target) == static_cast<ssize_t>(short_packet.size()));
    assert(live_socket.receive(1000, datagram) == ReceiveStatus::datagram);
    assert(datagram.bytes != nullptr && datagram.size == short_packet.size());
    assert(std::equal(short_packet.begin(), short_packet.end(), datagram.bytes));

    std::array<std::uint8_t, kGatewayDatagramLimit + 100> oversized{};
    assert(::sendto(gateway_peer,
                    oversized.data(),
                    oversized.size(),
                    0,
                    reinterpret_cast<const sockaddr*>(&target),
                    sizeof target) == static_cast<ssize_t>(oversized.size()));
    assert(live_socket.receive(1000, datagram) == ReceiveStatus::malformed);

    const std::array<std::uint8_t, 8> outgoing{{1, 2, 3, 4, 5, 6, 7, 8}};
    assert(live_socket.send_packet(outgoing.data(), outgoing.size()));
    std::array<std::uint8_t, outgoing.size()> received{};
    sockaddr_in sender{};
    socklen_t sender_size = sizeof sender;
    assert(::recvfrom(gateway_peer,
                      received.data(),
                      received.size(),
                      0,
                      reinterpret_cast<sockaddr*>(&sender),
                      &sender_size) == static_cast<ssize_t>(received.size()));
    assert(received == outgoing);
    assert(ntohs(sender.sin_port) == live_socket.local_port());
    assert(!live_socket.send_packet(nullptr, 1));
    ::close(unrelated_sender);
    ::close(gateway_peer);
}
