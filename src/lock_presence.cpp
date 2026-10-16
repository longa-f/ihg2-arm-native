#include "ihg2_arm/lock_presence.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ihg2::arm::presence {
namespace {

constexpr std::size_t kMacSize = 6;
constexpr std::uint16_t kEtherTypeIpv4 = 0x0800;
constexpr std::uint16_t kEtherTypeArp = 0x0806;
constexpr auto kProbeTimeout = std::chrono::milliseconds(600);

struct Interface {
    int index{};
    std::uint32_t address{};
    std::uint32_t mask{};
    MacAddress mac{};
};

struct ArpPayload {
    std::uint16_t hardware_type;
    std::uint16_t protocol_type;
    std::uint8_t hardware_size;
    std::uint8_t protocol_size;
    std::uint16_t operation;
    std::uint8_t sender_hardware[kMacSize];
    std::uint32_t sender_protocol;
    std::uint8_t target_hardware[kMacSize];
    std::uint32_t target_protocol;
} __attribute__((packed));

bool valid_unicast_mac(const MacAddress& mac) noexcept {
    return std::any_of(mac.begin(), mac.end(), [](std::uint8_t value) { return value != 0; }) &&
           (mac[0] & 1U) == 0;
}

int config_directory_fd(const std::string& directory) {
    const std::string path = directory + "/config";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) throw std::runtime_error("private bridge config directory is unavailable");
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 0077) != 0) {
        ::close(fd);
        throw std::runtime_error("bridge config directory is not private");
    }
    return fd;
}

std::optional<Interface> interface_for(std::uint32_t target) {
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) throw std::runtime_error("cannot enumerate network interfaces");

    std::optional<Interface> selected;
    unsigned selected_prefix = 0;
    for (auto* item = list; item; item = item->ifa_next) {
        if (!item->ifa_addr || !item->ifa_netmask || item->ifa_addr->sa_family != AF_INET ||
            (item->ifa_flags & IFF_UP) == 0 || (item->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
        const auto* mask = reinterpret_cast<const sockaddr_in*>(item->ifa_netmask);
        const auto host_mask = ntohl(mask->sin_addr.s_addr);
        const auto host_address = ntohl(address->sin_addr.s_addr);
        const auto host_target = ntohl(target);
        if ((host_address & host_mask) != (host_target & host_mask)) continue;

        const int index = static_cast<int>(::if_nametoindex(item->ifa_name));
        if (index <= 0) continue;
        const int ioctl_fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (ioctl_fd < 0) continue;
        ifreq request{};
        std::strncpy(request.ifr_name, item->ifa_name, sizeof request.ifr_name - 1);
        const bool mac_ok = ::ioctl(ioctl_fd, SIOCGIFHWADDR, &request) == 0;
        ::close(ioctl_fd);
        if (!mac_ok || request.ifr_hwaddr.sa_family != ARPHRD_ETHER) continue;

        unsigned prefix = 0;
        for (auto bits = host_mask; bits; bits >>= 1U) prefix += bits & 1U;
        if (prefix < selected_prefix) continue;

        Interface candidate;
        candidate.index = index;
        candidate.address = address->sin_addr.s_addr;
        candidate.mask = mask->sin_addr.s_addr;
        std::memcpy(candidate.mac.data(), request.ifr_hwaddr.sa_data, candidate.mac.size());
        selected = candidate;
        selected_prefix = prefix;
    }
    ::freeifaddrs(list);
    if (!selected) throw std::runtime_error("configured lock IP is not on a directly connected Ethernet subnet");

    const auto address = ntohl(selected->address);
    const auto mask = ntohl(selected->mask);
    const auto network = address & mask;
    const auto broadcast = network | ~mask;
    const auto host_target = ntohl(target);
    if (host_target == network || host_target == broadcast)
        throw std::runtime_error("configured lock IP is a subnet or broadcast address");
    return selected;
}

void send_arp_request(int fd, const Interface& interface, std::uint32_t target) {
    ArpPayload arp{};
    arp.hardware_type = htons(ARPHRD_ETHER);
    arp.protocol_type = htons(kEtherTypeIpv4);
    arp.hardware_size = 6;
    arp.protocol_size = 4;
    arp.operation = htons(ARPOP_REQUEST);
    std::memcpy(arp.sender_hardware, interface.mac.data(), interface.mac.size());
    arp.sender_protocol = interface.address;
    arp.target_protocol = target;

    sockaddr_ll destination{};
    destination.sll_family = AF_PACKET;
    destination.sll_protocol = htons(kEtherTypeArp);
    destination.sll_ifindex = interface.index;
    destination.sll_halen = 6;
    std::memset(destination.sll_addr, 0xFF, 6);
    if (::sendto(fd, &arp, sizeof arp, 0,
                 reinterpret_cast<const sockaddr*>(&destination), sizeof destination) < 0)
        throw std::runtime_error("cannot send the configured lock ARP probe");
}

std::optional<Config> read_config(int directory_fd) {
    const int fd = ::openat(directory_fd, "lock-sensor.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw std::runtime_error("cannot read lock sensor configuration");
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 0077) != 0 || st.st_size < 1 || st.st_size > 64) {
        ::close(fd);
        throw std::runtime_error("lock sensor configuration has unsafe ownership, permissions, or size");
    }
    char bytes[65]{};
    std::size_t used = 0;
    while (used < sizeof bytes) {
        const ssize_t count = ::read(fd, bytes + used, sizeof bytes - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += static_cast<std::size_t>(count);
    }
    ::close(fd);
    if (used == 0 || used >= sizeof bytes) throw std::runtime_error("lock sensor configuration is malformed");
    const std::string_view content(bytes, used);
    if (!content.starts_with("ip=") || content.size() < 10) throw std::runtime_error("lock sensor configuration is malformed");
    const auto newline = content.find('\n');
    if (newline == std::string_view::npos || newline + 5 >= content.size() ||
        content.substr(newline + 1, 4) != "mac=" || content.back() != '\n' ||
        content.find('\n', newline + 1) != content.size() - 1)
        throw std::runtime_error("lock sensor configuration is malformed");
    const auto config = make_config(content.substr(3, newline - 3), content.substr(newline + 5, content.size() - newline - 6));
    if (!config) throw std::runtime_error("lock sensor IP or MAC is invalid");
    return config;
}

} // namespace

std::optional<MacAddress> parse_mac(std::string_view text) noexcept {
    if (text.size() != 17) return std::nullopt;
    MacAddress mac{};
    for (std::size_t i = 0; i < mac.size(); ++i) {
        const std::size_t offset = i * 3;
        if (i && text[offset - 1] != ':') return std::nullopt;
        unsigned value = 0;
        for (std::size_t nibble = 0; nibble < 2; ++nibble) {
            const char c = text[offset + nibble];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
            value = value * 16U + digit;
        }
        mac[i] = static_cast<std::uint8_t>(value);
    }
    return mac;
}

std::string format_mac(const MacAddress& mac) {
    char text[18]{};
    std::snprintf(text, sizeof text, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

std::optional<Config> make_config(std::string_view ipv4, std::string_view mac_text) noexcept {
    if (ipv4.empty() || ipv4.size() >= INET_ADDRSTRLEN) return std::nullopt;
    char ip[INET_ADDRSTRLEN]{};
    std::memcpy(ip, ipv4.data(), ipv4.size());
    in_addr address{};
    if (::inet_pton(AF_INET, ip, &address) != 1) return std::nullopt;
    const auto host = ntohl(address.s_addr);
    const auto first = static_cast<unsigned>(host >> 24U);
    if (host == 0 || first == 0 || first == 127 || first >= 224) return std::nullopt;
    const auto mac = parse_mac(mac_text);
    if (!mac || !valid_unicast_mac(*mac)) return std::nullopt;
    return Config{address.s_addr, *mac};
}

std::optional<Config> load_config(const std::string& directory) {
    const int fd = config_directory_fd(directory);
    try {
        const auto result = read_config(fd);
        ::close(fd);
        return result;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

bool save_config(const std::string& directory, std::string_view ipv4, std::string_view mac) noexcept {
    const auto config = make_config(ipv4, mac);
    if (!config) return false;
    int dirfd = -1;
    int fd = -1;
    char temporary[48]{};
    try {
        dirfd = config_directory_fd(directory);
        std::snprintf(temporary, sizeof temporary, "lock-sensor.conf.tmp.%ld", static_cast<long>(::getpid()));
        fd = ::openat(dirfd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::runtime_error("cannot create private lock sensor configuration");
        char ip[INET_ADDRSTRLEN]{};
        in_addr address{config->ipv4};
        if (!::inet_ntop(AF_INET, &address, ip, sizeof ip)) throw std::runtime_error("cannot format lock IP address");
        const std::string content = std::string("ip=") + ip + "\nmac=" + format_mac(config->mac) + "\n";
        std::size_t written = 0;
        while (written < content.size()) {
            const ssize_t result = ::write(fd, content.data() + written, content.size() - written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) throw std::runtime_error("cannot write lock sensor configuration");
            written += static_cast<std::size_t>(result);
        }
        const bool metadata_ok = ::fchmod(fd, 0600) == 0 && ::fsync(fd) == 0;
        const int close_result = ::close(fd);
        fd = -1;
        if (!metadata_ok || close_result != 0) {
            throw std::runtime_error("cannot secure lock sensor configuration");
        }
        if (::renameat(dirfd, temporary, dirfd, "lock-sensor.conf") != 0 || ::fsync(dirfd) != 0)
            throw std::runtime_error("cannot commit lock sensor configuration");
        ::close(dirfd);
        return true;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        if (dirfd >= 0) {
            if (temporary[0]) (void)::unlinkat(dirfd, temporary, 0);
            ::close(dirfd);
        }
        return false;
    }
}

bool probe(const Config& config) {
    const auto interface = interface_for(config.ipv4);
    if (!interface) return false;
    const int fd = ::socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(kEtherTypeArp));
    if (fd < 0) throw std::runtime_error("cannot open ARP socket (root and SELinux packet-socket access required)");

    sockaddr_ll local{};
    local.sll_family = AF_PACKET;
    local.sll_protocol = htons(kEtherTypeArp);
    local.sll_ifindex = interface->index;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
        ::close(fd);
        throw std::runtime_error("cannot bind ARP socket to the configured lock subnet");
    }

    try {
        send_arp_request(fd, *interface, config.ipv4);
        const auto deadline = std::chrono::steady_clock::now() + kProbeTimeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            pollfd descriptor{fd, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
            if (ready == 0) break;
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("configured lock ARP receive failed");
            }
            if ((descriptor.revents & POLLIN) == 0) continue;
            ArpPayload arp{};
            const ssize_t bytes = ::recv(fd, &arp, sizeof arp, 0);
            if (bytes != static_cast<ssize_t>(sizeof arp) || ntohs(arp.hardware_type) != ARPHRD_ETHER ||
                ntohs(arp.protocol_type) != kEtherTypeIpv4 || arp.hardware_size != 6 ||
                arp.protocol_size != 4 || ntohs(arp.operation) != ARPOP_REPLY ||
                arp.sender_protocol != config.ipv4)
                continue;
            const bool match = std::memcmp(arp.sender_hardware, config.mac.data(), config.mac.size()) == 0;
            ::close(fd);
            return match;
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    return false;
}

bool Detector::observe(bool matched, std::chrono::steady_clock::time_point now) noexcept {
    if (!matched) return false;
    const bool changed = !online_;
    last_seen_ = now;
    online_ = true;
    return changed;
}

bool Detector::expire(std::chrono::steady_clock::time_point now, std::chrono::seconds timeout) noexcept {
    if (!online_ || now < last_seen_ || now - last_seen_ < timeout) return false;
    online_ = false;
    return true;
}

} // namespace ihg2::arm::presence
