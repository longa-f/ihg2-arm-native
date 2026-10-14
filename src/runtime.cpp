#include "ihg2_arm/runtime.hpp"
#include "ihg2_arm/runtime_c.h"

#include "ihg2_arm/homekit_color_temperature.hpp"
#include "ihg2_arm/homekit_thermostat.hpp"
#include "ihg2_arm/gateway_socket.hpp"
#include "ihg2_arm/homekit_cover.hpp"
#include "ihg2_arm/panel_config.hpp"
#include "ihg2_arm/vendor_protocol/protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace ihg2::arm;
using Clock = std::chrono::steady_clock;

struct Device {
    std::string id, name, platform, target, parent, group, control, sequence;
    std::vector<std::string> properties, aliases;
    std::optional<std::uint16_t> last_sequence;
    std::optional<int> requested_cover_target;
    std::uint8_t temperature_display_units{};
    Clock::time_point cover_target_issued{};
    Clock::time_point cover_target_deadline{};
    Clock::time_point sequence_seen{};
    Json values = Json::object();
};
struct Pending {
    enum class Kind { heartbeat, query, control } kind{};
    std::uint16_t count{};
    std::string target;
    Clock::time_point deadline{};
};

bool tcp_listener_responds(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const int old_flags = ::fcntl(fd, F_GETFL, 0);
    if (old_flags < 0 || ::fcntl(fd, F_SETFL, old_flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return false;
    }
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int result = ::connect(fd, reinterpret_cast<const sockaddr*>(&peer), sizeof peer);
    bool connected = result == 0;
    if (!connected && errno == EINPROGRESS) {
        pollfd descriptor{fd, POLLOUT, 0};
        if (::poll(&descriptor, 1, 500) > 0) {
            int error = 0;
            socklen_t size = sizeof error;
            connected = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0;
        }
    }
    ::close(fd);
    return connected;
}

bool udp_port_bound(std::uint16_t port) {
    const auto expected = static_cast<unsigned>(port);
    for (const char* path : {"/proc/net/udp", "/proc/net/udp6"}) {
        std::ifstream input(path);
        std::string line;
        (void)std::getline(input, line);
        while (std::getline(input, line)) {
            std::istringstream row(line);
            std::string slot, local;
            if (!(row >> slot >> local)) continue;
            const auto colon = local.rfind(':');
            if (colon == std::string::npos) continue;
            char* end = nullptr;
            const auto value = std::strtoul(local.c_str() + colon + 1, &end, 16);
            if (end != local.c_str() + colon + 1 && *end == '\0' && value == expected) return true;
        }
    }
    return false;
}

bool boot_hook_installed() {
    std::ifstream input("/system/bin/install-recovery.sh");
    std::string line;
    bool marker_found = false;
    while (std::getline(input, line))
        if (line.find("IHG2_ARM_NATIVE_BOOT_HOOK_V1") != std::string::npos) marker_found = true;
    if (!marker_found) return false;

    char context[256]{};
    ssize_t size = ::getxattr("/system/bin/install-recovery.sh", "security.selinux",
                              context, sizeof(context) - 1);
    if (size <= 0) return false;
    if (context[size - 1] == '\0') --size;
    return std::string(context, static_cast<std::size_t>(size)) == "u:object_r:install_recovery_exec:s0";
}

std::optional<std::string> read_pairing_code(const std::string& directory) {
    const std::string path = directory + "/config/pairing-code.txt";
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & 0077) != 0 || st.st_size != 11) {
        ::close(fd);
        return std::nullopt;
    }

    char bytes[12]{};
    std::size_t used = 0;
    while (used < sizeof(bytes)) {
        const ssize_t count = ::read(fd, bytes + used, sizeof(bytes) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            ::close(fd);
            return std::nullopt;
        }
        if (count == 0) break;
        used += static_cast<std::size_t>(count);
    }
    ::close(fd);
    if (used != 11 || bytes[10] != '\n') return std::nullopt;

    for (std::size_t i = 0; i < 10; ++i) {
        if (i == 3 || i == 6) {
            if (bytes[i] != '-') return std::nullopt;
        } else if (bytes[i] < '0' || bytes[i] > '9') {
            return std::nullopt;
        }
    }
    return std::string(bytes, 10);
}

bool gateway_heartbeat_probe(const PanelSdkConfig& panel, const std::string& gateway_ip) {
    GatewaySocket socket;
    constexpr std::uint16_t count = 0x6d31;
    const Identity identity{panel.gateway_mac, 1, 0x274a};
    if (!socket.open(gateway_ip.c_str(), 80)) return false;
    const auto request = encode_packet(Json(nullptr), panel.gateway_key, identity, 0x0069, count, 0x0b02, 1);
    if (!socket.send_packet(request.data(), request.size())) return false;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        GatewayDatagramView datagram{};
        const auto wait = static_cast<std::uint32_t>(std::max<std::int64_t>(1, remaining.count()));
        if (socket.receive(wait, datagram) != ReceiveStatus::datagram) return false;
        try {
            const auto frame = decode_packet({datagram.bytes, datagram.size}, panel.gateway_key, identity, true, true);
            if (frame.command == response_heartbeat && frame.count == count) return true;
        } catch (const ProtocolError&) {
            // Ignore unrelated datagrams until the probe deadline.
        }
    }
    return false;
}

void clear_secret(Key& value) noexcept {
    volatile std::uint8_t* p = value.data();
    for (std::size_t i = 0; i < value.size(); ++i) p[i] = 0;
}

std::optional<bool> power_value(const Json& value) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_integer()) {
        if (value == 0) return false;
        if (value == 1) return true;
    }
    if (value.is_string()) {
        const auto s = value.get<std::string>();
        if (s == "ON" || s == "on" || s == "1" || s == "true" || s == "WET" || s == "DETECTED") return true;
        if (s == "OFF" || s == "off" || s == "0" || s == "false" || s == "DRY" || s == "CLEAR") return false;
    }
    return std::nullopt;
}

std::optional<int> integer_value(const Json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(INT_MAX)) return std::nullopt;
        return static_cast<int>(number);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < INT_MIN || number > INT_MAX) return std::nullopt;
        return static_cast<int>(number);
    }
    if (value.is_number_float()) {
        const auto number = value.get<double>();
        if (!std::isfinite(number) || number < INT_MIN || number > INT_MAX) return std::nullopt;
        return static_cast<int>(std::lround(number));
    }
    if (value.is_string()) {
        const auto s = value.get<std::string>();
        if (s.empty() || s.size() > 12) return std::nullopt;
        char* end = nullptr;
        errno = 0;
        const auto number = std::strtol(s.c_str(), &end, 10);
        if (errno || end != s.c_str() + s.size() || number < INT_MIN || number > INT_MAX) return std::nullopt;
        return static_cast<int>(number);
    }
    return std::nullopt;
}

std::optional<double> decimal_value(const Json& value) {
    if (value.is_number()) {
        const auto number = value.get<double>();
        return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
    }
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.empty() || text.size() > 16) return std::nullopt;
        char* end = nullptr;
        errno = 0;
        const auto number = std::strtod(text.c_str(), &end);
        if (errno || end != text.c_str() + text.size() || !std::isfinite(number)) return std::nullopt;
        return number;
    }
    return std::nullopt;
}

bool has_property(const Device& device, const char* property) {
    return std::find(device.properties.begin(), device.properties.end(), property) != device.properties.end();
}

class Runtime final {
public:
    ~Runtime() { clear_secret(key_); }

    bool initialize(const char* directory) {
        if (!directory || !*directory) return false;
        try {
            auto panel = load_panel_sdk_config();
            gateway_ip_ = gateway_ipv4_from_arp_file(panel.gateway_mac);
            gateway_port_ = 80;
            identity_.mac = panel.gateway_mac;
            identity_.device_id = 1;
            identity_.devtype = 0x274a;
            key_ = panel.gateway_key;
            auto inventory = std::move(panel.inventory);
            allow_control_ = true;
            homekit_name_ = "IHG2 Local Bridge";
            homekit_port_ = 53760;
            std::fprintf(stderr, "ihg2-arm: loaded %zu accessories from panel SDK auto-discovery\n",
                         inventory.size());
            if (!inventory.is_array() || inventory.empty() || inventory.size() > 32) return false;
            std::set<std::string> ids;
            for (const auto& item : inventory) {
                Device d;
                d.id = item.at("unique_id").get<std::string>();
                d.name = item.at("name").get<std::string>();
                d.platform = item.at("platform").get<std::string>();
                d.target = item.at("target_did").get<std::string>();
                d.parent = item.value("parent_did", d.target);
                d.group = item.value("groupid", std::string{});
                d.control = item.at("control_key").get<std::string>();
                d.sequence = item.value("sequence_key", std::string{});
                if (d.name.empty() || d.name.size() > 63 || d.id.empty() || !ids.insert(d.id).second ||
                    d.target.empty() || d.target.size() > 128 || d.control.empty()) return false;
                for (const auto& p : item.at("properties")) d.properties.push_back(p.get<std::string>());
                if (item.contains("aliases")) for (const auto& a : item.at("aliases")) d.aliases.push_back(a.get<std::string>());
                devices_.push_back(std::move(d));
            }
            for (const auto& d : devices_) targets_set_.insert(d.target);
            targets_.assign(targets_set_.begin(), targets_set_.end());
            if (!socket_.open(gateway_ip_.c_str(), gateway_port_)) return false;
            next_heartbeat_ = Clock::now();
            next_poll_ = Clock::now() + std::chrono::milliseconds(100);
            initialized_ = true;
            std::fprintf(stderr, "ihg2-arm: loaded %zu accessories; UDP source port %u\n", devices_.size(), socket_.local_port());
            return true;
        } catch (...) {
            return false;
        }
    }

    int doctor(const char* directory) {
        std::printf("IHG2 ARM bridge doctor (sends one heartbeat probe; no device controls)\n");
        bool healthy = true;
        const auto check = [&](bool ok, const char* label) {
            std::printf("[%s] %s\n", ok ? "OK" : "FAIL", label);
            if (!ok) healthy = false;
        };
        check(::geteuid() == 0, "root access");
        if (!directory || !*directory) {
            check(false, "bridge data directory");
            return 1;
        }

        PanelSdkConfig panel;
        bool panel_ok = false;
        try {
            panel = load_panel_sdk_config();
            panel_ok = true;
            check(true, "panel SDK key and gateway identity readable (values hidden)");
            std::printf("[OK] supported accessory inventory: %zu\n", panel.inventory.size());
        } catch (...) {
            check(false, "panel SDK key, gateway identity, and device inventory");
        }

        std::string gateway_ip;
        bool neighbor_ok = false;
        if (panel_ok) {
            try {
                gateway_ip = gateway_ipv4_from_arp_file(panel.gateway_mac);
                neighbor_ok = true;
                check(true, "gateway address resolved from the panel neighbor table");
            } catch (...) {
                check(false, "gateway address resolved from the panel neighbor table");
            }
        } else {
            check(false, "gateway address resolved from the panel neighbor table");
        }

        check(tcp_listener_responds(53760), "HomeKit TCP listener");
        check(udp_port_bound(5353), "mDNS UDP listener");
        check(boot_hook_installed(), "Android boot hook and SELinux execution label");
        bool no_legacy_config = true;
        const std::string base = directory;
        for (const auto& path : {base + "/bridge.json", base + "/gateway.key",
                                 base + "/config/inventory.json"}) {
            struct stat legacy_stat{};
            if (::lstat(path.c_str(), &legacy_stat) == 0 || errno != ENOENT) no_legacy_config = false;
        }
        check(no_legacy_config, "obsolete manual gateway config removed");
        struct stat store_stat{};
        const bool store_ok = ::stat((std::string(directory) + "/.hap_store").c_str(), &store_stat) == 0 &&
                              S_ISDIR(store_stat.st_mode) && store_stat.st_uid == ::geteuid() &&
                              (store_stat.st_mode & 0077) == 0;
        check(store_ok, "private HomeKit pairing store");

        const bool heartbeat_ok = panel_ok && neighbor_ok && gateway_heartbeat_probe(panel, gateway_ip);
        check(heartbeat_ok, "gateway UDP heartbeat reply");
        std::printf("Doctor result: %s\n", healthy ? "healthy" : "attention required");
        if (healthy) {
            if (const auto code = read_pairing_code(base))
                std::printf("[PAIRING] HomeKit setup code: %s\n", code->c_str());
            else
                std::printf("[PAIRING] HomeKit setup code unavailable (protected code file missing or invalid).\n");
        }
        return healthy ? 0 : 1;
    }

    void poll() {
        if (!initialized_) return;
        const auto now = Clock::now();
        GatewayDatagramView datagram{};
        for (int i = 0; i < 8; ++i) {
            const auto status = socket_.receive(0, datagram);
            if (status != ReceiveStatus::datagram) break;
            try { handle(decode_packet({datagram.bytes, datagram.size}, key_, identity_, true, true), now); }
            catch (...) { ++bad_packets_; }
        }
        if (pending_ && now >= pending_->deadline) {
            if (pending_->kind == Pending::Kind::control && is_cover_target(pending_->target))
                std::fprintf(stderr, "ihg2-arm: gateway curtain command timed out\n");
            quarantined_[pending_->count & 0x7FFF] = false;
            if (pending_->kind == Pending::Kind::control) query_after_control_ = pending_->target;
            pending_.reset();
        }
        if (pending_) return;
        if (!queue_empty()) {
            send_next(now);
            return;
        }
        if (now >= next_heartbeat_) {
            send(Pending::Kind::heartbeat, {}, Json(nullptr), 0x69, 1, now);
            next_heartbeat_ = now + std::chrono::seconds(20);
            return;
        }
        if (!query_after_control_.empty()) {
            const auto target = std::move(query_after_control_);
            query_after_control_.clear();
            send(Pending::Kind::query, target, Json{{"did", target}}, 0x6a, 0, now);
            return;
        }
        if (now >= next_poll_ && !targets_.empty()) {
            const auto target = targets_[target_index_++];
            const bool end_of_cycle = target_index_ == targets_.size();
            if (end_of_cycle) target_index_ = 0;
            send(Pending::Kind::query, target, Json{{"did", target}}, 0x6a, 0, now);
            // Keep the conservative 250 ms request spacing, but do not leave a
            // long blind window after walking all gateway targets. With the
            // installed 23-target inventory, a 15 s cycle pause made external
            // switch changes take over 20 s to appear in HomeKit.
            next_poll_ = now + (end_of_cycle ? std::chrono::seconds(1) : std::chrono::milliseconds(250));
        }
    }

    std::size_t count() const { return devices_.size(); }
    const char* name(std::size_t i) const { return i < devices_.size() ? devices_[i].name.c_str() : ""; }
    const char* homekit_name() const { return homekit_name_.c_str(); }
    std::uint16_t homekit_port() const { return homekit_port_; }
    bool is_light(std::size_t i) const { return i < devices_.size() && devices_[i].platform == "light"; }
    bool is_cover(std::size_t i) const { return i < devices_.size() && devices_[i].platform == "cover"; }
    bool is_climate(std::size_t i) const { return i < devices_.size() && devices_[i].platform == "climate"; }
    bool can_write(std::size_t i) const { return allow_control_ && i < devices_.size() && devices_[i].platform != "binary_sensor"; }
    bool has_brightness(std::size_t i) const { return i < devices_.size() && has_property(devices_[i], "brightness"); }
    bool has_color_temperature(std::size_t i) const { return i < devices_.size() && has_property(devices_[i], "colortemp"); }
    bool has_cover_position(std::size_t i) const { return i < devices_.size() && is_cover(i) && has_property(devices_[i], "curtain_screentargetpos"); }
    std::uint64_t aid(std::size_t i) const {
        if (i >= devices_.size()) return 0;
        std::uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : devices_[i].id) { h ^= c; h *= 1099511628211ULL; }
        h &= 0x7FFFFFFFFFFFFFFFULL;
        return h < 2 ? h + 2 : h;
    }
    bool read_on(std::uint64_t aid) const {
        const auto i = index_for_aid(aid);
        if (i >= devices_.size()) return false;
        const auto& d = devices_[i];
        if (d.platform == "cover") {
            const auto pos = property_int(d, "curtain_screentargetpos");
            return pos && *pos > 0;
        }
        if (d.values.contains(d.control)) return power_value(d.values[d.control]).value_or(false);
        return false;
    }
    bool write_on(std::uint64_t aid, bool on) {
        auto* device = device_for_aid(aid);
        if (!device || !can_write_device(*device)) return false;
        auto& d = *device;
        Json values;
        if (d.platform == "cover") values = {{"curtain_ctrl", on ? 1 : 2}};
        else values = {{d.control, on ? 1 : 0}};
        return enqueue(d, std::move(values));
    }
    int read_brightness(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || !has_property(*d, "brightness")) return 100;
        const auto value = property_int(*d, "brightness");
        return value ? std::clamp(*value, 0, 100) : 100;
    }
    bool write_brightness(std::uint64_t aid, int percent) {
        auto* d = device_for_aid(aid);
        if (!d || !can_write_device(*d) || !has_property(*d, "brightness") || percent < 0 || percent > 100) return false;
        return enqueue(*d, {{"brightness", percent}});
    }
    std::uint32_t read_color_temperature(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || !has_property(*d, "colortemp")) return 200;
        const auto raw = property_int(*d, "colortemp");
        const auto value = raw ? homekit_mirek_from_gateway_colortemp(*raw) : std::nullopt;
        return value ? std::clamp(*value, 50U, 400U) : 200U;
    }
    bool write_color_temperature(std::uint64_t aid, std::uint32_t mirek) {
        auto* d = device_for_aid(aid);
        if (!d || !can_write_device(*d) || !has_property(*d, "colortemp") || mirek < 50 || mirek > 400) return false;
        const auto value = gateway_colortemp_from_homekit_mirek(mirek);
        return value && enqueue(*d, {{"colortemp", *value}});
    }
    int read_cover_current_position(std::uint64_t aid) const {
        return cover_position(aid);
    }
    int read_cover_target_position(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || d->platform != "cover") return 0;
        if (d->requested_cover_target) return *d->requested_cover_target;
        return homekit_cover_target_from_motion(cover_position(*d), cover_state(*d));
    }
    int read_cover_position_state(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || d->platform != "cover") return 2;
        if (d->requested_cover_target)
            return homekit_cover_state_for_target(cover_position(*d), *d->requested_cover_target);
        return cover_state(*d);
    }
    bool write_cover_target_position(std::uint64_t aid, int percent) {
        const auto i = index_for_aid(aid);
        if (i >= devices_.size()) return false;
        auto* d = &devices_[i];
        if (!can_write_device(*d) || d->platform != "cover" ||
            !has_property(*d, "curtain_screentargetpos") || percent < 0 || percent > 100) return false;
        const int old_target = read_cover_target_position(aid);
        const int old_state = read_cover_position_state(aid);
        if (!enqueue(*d, {{"curtain_screentargetpos", percent}})) return false;
        std::fprintf(stderr, "ihg2-arm: curtain target queued position=%d\n", percent);
        d->requested_cover_target = percent;
        d->cover_target_issued = Clock::now();
        d->cover_target_deadline = d->cover_target_issued + std::chrono::seconds(90);
        std::uint32_t changed_mask = 0;
        if (old_target != percent) changed_mask |= IHG2_HAP_STATE_COVER_TARGET;
        if (old_state != read_cover_position_state(aid))
            changed_mask |= IHG2_HAP_STATE_COVER_POSITION_STATE;
        if (changed_mask) ihg2_hap_state_changed(i, changed_mask);
        return true;
    }
    std::uint8_t read_thermostat_target_state(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || d->platform != "climate" || !climate_powered(*d)) return 0;
        const auto mode = ac_mode(*d);
        const auto state = mode ? homekit_target_state_from_gateway_mode(*mode) : std::nullopt;
        return state.value_or(3);
    }
    std::uint8_t read_thermostat_current_state(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || d->platform != "climate") return 0;
        const auto mode = ac_mode(*d);
        if (!mode) return 0;
        return homekit_current_state_from_gateway_mode(
                climate_powered(*d), *mode,
                climate_temperature(*d, "insidetemp", 20.0F),
                climate_temperature(*d, "ac_temp", 22.0F));
    }
    bool write_thermostat_target_state(std::uint64_t aid, std::uint8_t state) {
        auto* d = device_for_aid(aid);
        if (!d || d->platform != "climate" || !can_write_device(*d)) return false;
        if (state == 0) return enqueue(*d, {{d->control, 0}});
        const auto mode = gateway_mode_from_homekit_target_state(state);
        if (!mode || !has_property(*d, "ac_mode")) return false;
        return enqueue(*d, {{d->control, 1}, {"ac_mode", *mode}});
    }
    float read_current_temperature(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        return !d || d->platform != "climate" ? 20.0F : climate_temperature(*d, "insidetemp", 20.0F);
    }
    float read_target_temperature(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        if (!d || d->platform != "climate") return 22.0F;
        return std::clamp(climate_temperature(*d, "ac_temp", 22.0F), 10.0F, 38.0F);
    }
    bool write_target_temperature(std::uint64_t aid, float temperature) {
        auto* d = device_for_aid(aid);
        if (!d || d->platform != "climate" || !can_write_device(*d) ||
            !has_property(*d, "ac_temp") || !std::isfinite(temperature) || temperature < 10.0F || temperature > 38.0F)
            return false;
        return enqueue(*d, {{"ac_temp", static_cast<int>(std::lround(temperature))}});
    }
    std::uint8_t read_temperature_display_units(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        return !d || d->platform != "climate" ? 0 : d->temperature_display_units;
    }
    bool write_temperature_display_units(std::uint64_t aid, std::uint8_t units) {
        auto* d = device_for_aid(aid);
        if (!d || d->platform != "climate" || units > 1) return false;
        d->temperature_display_units = units;
        return true;
    }
    void set_hap_port() const {}

private:
    static std::optional<int> ac_mode(const Device& d) {
        if (!d.values.contains("ac_mode")) return std::nullopt;
        const auto& value = d.values["ac_mode"];
        if (const auto raw = integer_value(value)) return raw;
        if (!value.is_string()) return std::nullopt;
        auto text = value.get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        if (text == "AUTO") return 0;
        if (text == "COOL" || text == "COOLING") return 1;
        if (text == "DRY" || text == "DEHUMIDIFY") return 2;
        if (text == "FAN" || text == "FAN_ONLY") return 3;
        if (text == "HEAT" || text == "HEATING") return 4;
        return std::nullopt;
    }
    static bool climate_powered(const Device& d) {
        return d.values.contains(d.control) && power_value(d.values[d.control]).value_or(false);
    }
    static float climate_temperature(const Device& d, const char* property, float fallback) {
        if (!d.values.contains(property)) return fallback;
        const auto value = decimal_value(d.values[property]);
        if (!value || *value < 0.0 || *value > 100.0) return fallback;
        return static_cast<float>(*value);
    }
    static std::optional<int> property_int(const Device& d, const char* key) {
        if (!d.values.contains(key)) return std::nullopt;
        auto value = integer_value(d.values[key]);
        if (!value || *value < 0 || *value > 10000) return std::nullopt;
        return value;
    }
    static int cover_state(const Device& d) {
        if (!d.values.contains("motostatus")) return 2;
        const auto& value = d.values["motostatus"];
        if (const auto raw = integer_value(value)) {
            const auto state = homekit_cover_state_from_gateway(*raw);
            return state ? *state : 2;
        }
        if (!value.is_string()) return 2;
        auto text = value.get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        if (text == "OPENING" || text == "OPEN" || text == "UP") return 1;
        if (text == "CLOSING" || text == "CLOSE" || text == "DOWN") return 0;
        return 2;
    }
    int cover_position(std::uint64_t aid) const {
        const auto* d = device_for_aid(aid);
        return d ? cover_position(*d) : 0;
    }
    static int cover_position(const Device& d) {
        if (d.platform != "cover") return 0;
        const auto value = property_int(d, "curtain_screentargetpos");
        return value ? std::clamp(*value, 0, 100) : 0;
    }
    Device* device_for_aid(std::uint64_t value) {
        const auto i = index_for_aid(value);
        return i < devices_.size() ? &devices_[i] : nullptr;
    }
    const Device* device_for_aid(std::uint64_t value) const {
        const auto i = index_for_aid(value);
        return i < devices_.size() ? &devices_[i] : nullptr;
    }
    bool can_write_device(const Device& device) const {
        return allow_control_ && device.platform != "binary_sensor";
    }
    bool enqueue(Device& d, Json values) {
        if (queue_size_ == commands_.size()) return false;
        commands_[queue_tail_] = {d.target, std::move(values)};
        queue_tail_ = (queue_tail_ + 1) % commands_.size();
        ++queue_size_;
        return true;
    }
    struct Command { std::string target; Json values; };
    std::size_t index_for_aid(std::uint64_t value) const {
        for (std::size_t i = 0; i < devices_.size(); ++i) if (aid(i) == value) return i;
        return devices_.size();
    }
    bool queue_empty() const { return queue_size_ == 0; }
    bool is_cover_target(const std::string& target) const {
        return std::any_of(devices_.begin(), devices_.end(), [&](const Device& device) {
            return device.platform == "cover" && device.target == target;
        });
    }
    void clear_requested_cover_targets(const std::string& target) {
        for (std::size_t i = 0; i < devices_.size(); ++i) {
            auto& d = devices_[i];
            if (d.platform != "cover" || d.target != target || !d.requested_cover_target) continue;
            const int old_target = *d.requested_cover_target;
            const int old_state = homekit_cover_state_for_target(cover_position(d), old_target);
            d.requested_cover_target.reset();
            const int new_target = homekit_cover_target_from_motion(cover_position(d), cover_state(d));
            const int new_state = cover_state(d);
            std::uint32_t changed_mask = 0;
            if (old_target != new_target) changed_mask |= IHG2_HAP_STATE_COVER_TARGET;
            if (old_state != new_state) changed_mask |= IHG2_HAP_STATE_COVER_POSITION_STATE;
            if (changed_mask) ihg2_hap_state_changed(i, changed_mask);
        }
    }
    void send(Pending::Kind kind, const std::string& target, const Json& payload, std::uint16_t command, std::uint16_t flags, Clock::time_point now) {
        for (std::size_t tries = 0; tries < 32768; ++tries) {
            const auto count = sequence_++;
            sequence_ |= 0x8000;
            if (!quarantined_[count & 0x7FFF]) {
                const auto bytes = encode_packet(payload, key_, identity_, command, count, 0x0b02, flags);
                if (!socket_.send_packet(bytes.data(), bytes.size())) {
                    if (kind == Pending::Kind::control && is_cover_target(target)) {
                        std::fprintf(stderr, "ihg2-arm: gateway curtain command send failed\n");
                        clear_requested_cover_targets(target);
                    }
                    return;
                }
                if (kind == Pending::Kind::control && is_cover_target(target)) {
                    const int position = payload.contains("curtain_screentargetpos")
                            ? integer_value(payload["curtain_screentargetpos"]).value_or(-1) : -1;
                    std::fprintf(stderr, "ihg2-arm: gateway curtain command sent position=%d seq=%u\n",
                                 position, static_cast<unsigned>(count));
                }
                quarantined_[count & 0x7FFF] = true;
                pending_ = Pending{kind, count, target, now + std::chrono::seconds(2)};
                return;
            }
        }
    }
    void send_next(Clock::time_point now) {
        auto command = std::move(commands_[queue_head_]);
        queue_head_ = (queue_head_ + 1) % commands_.size();
        --queue_size_;
        command.values["did"] = command.target;
        send(Pending::Kind::control, command.target, command.values, 0x6a, 0, now);
    }
    std::vector<std::size_t> resolve(const Json& report) const {
        if (!report.is_object() || !report.contains("did") || !report["did"].is_string()) return {};
        const auto did = report["did"].get<std::string>();
        std::vector<std::size_t> found;
        for (std::size_t i = 0; i < devices_.size(); ++i) {
            const auto& d = devices_[i];
            bool match = did == d.target || did == d.parent ||
                         (!d.group.empty() && (did == d.target + "_" + d.group || did == d.parent + "_" + d.group)) ||
                         std::find(d.aliases.begin(), d.aliases.end(), did) != d.aliases.end();
            if (match && report.contains("sdid")) match = report["sdid"].is_string() && report["sdid"] == d.target;
            if (match) found.push_back(i);
        }
        if (found.size() > 1) {
            const auto target = devices_[found.front()].target;
            if (std::any_of(found.begin(), found.end(), [&](auto i) { return devices_[i].target != target; })) return {};
            if (!report.contains("sdid") && !devices_[found.front()].group.empty()) return {};
        }
        return found;
    }
    void handle(const Frame& frame, Clock::time_point now) {
        if (frame.command != response_get && frame.command != response_heartbeat) return;
        if (pending_ && pending_->count == frame.count) {
            if (frame.command == response_heartbeat && pending_->kind != Pending::Kind::heartbeat) return;
            if (frame.command == response_get && pending_->kind == Pending::Kind::heartbeat) return;
            const auto kind = pending_->kind;
            const auto target = pending_->target;
            if (kind == Pending::Kind::control && is_cover_target(target))
                std::fprintf(stderr, "ihg2-arm: gateway curtain command response error=%u\n",
                             static_cast<unsigned>(frame.error));
            pending_.reset();
            quarantined_[frame.count & 0x7FFF] = false;
            if (kind == Pending::Kind::control) {
                if (frame.error != 0 && is_cover_target(target)) clear_requested_cover_targets(target);
                query_after_control_ = target;
            }
        }
        if (!frame.payload.is_object() || !frame.payload.contains("did")) return;
        const auto indices = resolve(frame.payload);
        for (auto i : indices) {
            auto& d = devices_[i];
            if (!d.sequence.empty() && frame.payload.contains(d.sequence)) {
                const auto& sequence = frame.payload[d.sequence];
                if (!sequence.is_number_integer()) continue;
                const auto raw = sequence.get<std::int64_t>();
                if (raw < 0 || raw > 65535) continue;
                const auto current = static_cast<std::uint16_t>(raw);
                if (d.last_sequence && static_cast<std::uint16_t>(current - *d.last_sequence) > 32767 &&
                    now - d.sequence_seen < std::chrono::minutes(5)) continue;
                d.last_sequence = current;
                d.sequence_seen = now;
            }
            const bool is_cover = d.platform == "cover";
            const int old_cover_target = is_cover
                    ? (d.requested_cover_target ? *d.requested_cover_target
                                                : homekit_cover_target_from_motion(cover_position(d), cover_state(d)))
                    : 0;
            const int old_cover_state = is_cover ? cover_state(d) : 2;
            std::uint32_t changed_mask = 0;
            for (const auto& prop : d.properties) {
                if (!frame.payload.contains(prop) || !(frame.payload[prop].is_boolean() || frame.payload[prop].is_number() || frame.payload[prop].is_string())) continue;
                if (prop == d.control && !power_value(frame.payload[prop])) continue;
                if (!d.values.contains(prop) || d.values[prop] != frame.payload[prop]) {
                    d.values[prop] = frame.payload[prop];
                    if (prop == d.control) changed_mask |= IHG2_HAP_STATE_ON;
                    if (prop == "brightness") changed_mask |= IHG2_HAP_STATE_BRIGHTNESS;
                    if (prop == "colortemp") changed_mask |= IHG2_HAP_STATE_COLOR_TEMPERATURE;
                    if (prop == "curtain_screentargetpos")
                        changed_mask |= IHG2_HAP_STATE_COVER_CURRENT | IHG2_HAP_STATE_ON;
                    if (d.platform == "climate") {
                        if (prop == d.control || prop == "ac_mode")
                            changed_mask |= IHG2_HAP_STATE_THERMOSTAT_CURRENT_STATE |
                                            IHG2_HAP_STATE_THERMOSTAT_TARGET_STATE;
                        if (prop == "ac_temp")
                            changed_mask |= IHG2_HAP_STATE_TARGET_TEMPERATURE |
                                            IHG2_HAP_STATE_THERMOSTAT_CURRENT_STATE;
                        if (prop == "insidetemp")
                            changed_mask |= IHG2_HAP_STATE_CURRENT_TEMPERATURE |
                                            IHG2_HAP_STATE_THERMOSTAT_CURRENT_STATE;
                    }
                }
            }
            if (!frame.payload.contains(d.control) && (d.platform == "light" || d.platform == "switch") &&
                (frame.payload.contains("pwr") || frame.payload.contains("powerState"))) {
                const auto key = frame.payload.contains("pwr") ? "pwr" : "powerState";
                if (!d.values.contains(d.control) || d.values[d.control] != frame.payload[key]) {
                    d.values[d.control] = frame.payload[key];
                    changed_mask |= IHG2_HAP_STATE_ON;
                }
            }
            if (is_cover) {
                const int position = cover_position(d);
                const int state = cover_state(d);
                if (d.requested_cover_target &&
                    (position == *d.requested_cover_target || now >= d.cover_target_deadline ||
                     (state == 2 && now - d.cover_target_issued >= std::chrono::seconds(5)))) {
                    d.requested_cover_target.reset();
                }
                const int new_cover_target = d.requested_cover_target
                        ? *d.requested_cover_target : homekit_cover_target_from_motion(position, state);
                if (new_cover_target != old_cover_target) changed_mask |= IHG2_HAP_STATE_COVER_TARGET;
                if (state != old_cover_state) changed_mask |= IHG2_HAP_STATE_COVER_POSITION_STATE;
                if (changed_mask && (frame.payload.contains("curtain_screentargetpos") ||
                                     frame.payload.contains("motostatus")))
                    std::fprintf(stderr, "ihg2-arm: curtain state update position=%d motion=%d\n",
                                 position, state);
            }
            if (changed_mask) ihg2_hap_state_changed(i, changed_mask);
        }
        (void)now;
    }

    std::string gateway_ip_, homekit_name_;
    std::uint16_t gateway_port_{80}, homekit_port_{53760}, sequence_{0x8000};
    Key key_{};
    Identity identity_{};
    GatewaySocket socket_;
    std::vector<Device> devices_;
    std::set<std::string> targets_set_;
    std::vector<std::string> targets_;
    std::size_t target_index_{};
    std::array<Command, 16> commands_{};
    std::size_t queue_head_{}, queue_tail_{}, queue_size_{};
    std::array<bool, 32768> quarantined_{};
    std::optional<Pending> pending_;
    std::string query_after_control_;
    Clock::time_point next_heartbeat_{}, next_poll_{};
    std::uint64_t bad_packets_{};
    bool allow_control_{};
    bool initialized_{};
};

Runtime runtime;
}

extern "C" int ihg2_arm_initialize(const char* directory) { return runtime.initialize(directory) ? 1 : 0; }
extern "C" int ihg2_arm_doctor(const char* directory) { return runtime.doctor(directory); }
extern "C" int ihg2_arm_panel_adoption_probe(std::size_t* accessory_count) {
    if (accessory_count) *accessory_count = 0;
    try {
        auto panel = ihg2::arm::load_panel_sdk_config();
        (void)ihg2::arm::gateway_ipv4_from_arp_file(panel.gateway_mac);
        if (accessory_count) *accessory_count = panel.inventory.size();
        return 1;
    } catch (...) {
        return 0;
    }
}
extern "C" void ihg2_arm_poll(void) { runtime.poll(); }
extern "C" std::size_t ihg2_arm_device_count(void) { return runtime.count(); }
extern "C" std::uint64_t ihg2_arm_device_aid(std::size_t i) { return runtime.aid(i); }
extern "C" const char* ihg2_arm_device_name(std::size_t i) { return runtime.name(i); }
extern "C" const char* ihg2_arm_homekit_name(void) { return runtime.homekit_name(); }
extern "C" std::uint16_t ihg2_arm_homekit_port(void) { return runtime.homekit_port(); }
extern "C" int ihg2_arm_device_is_light(std::size_t i) { return runtime.is_light(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_is_cover(std::size_t i) { return runtime.is_cover(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_is_climate(std::size_t i) { return runtime.is_climate(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_can_write(std::size_t i) { return runtime.can_write(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_has_brightness(std::size_t i) { return runtime.has_brightness(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_has_color_temperature(std::size_t i) { return runtime.has_color_temperature(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_has_cover_position(std::size_t i) { return runtime.has_cover_position(i) ? 1 : 0; }
extern "C" int ihg2_arm_device_read_on(std::uint64_t aid) { return runtime.read_on(aid) ? 1 : 0; }
extern "C" int ihg2_arm_device_write_on(std::uint64_t aid, int on) { return runtime.write_on(aid, on != 0) ? 1 : 0; }
extern "C" int ihg2_arm_device_read_brightness(std::uint64_t aid) { return runtime.read_brightness(aid); }
extern "C" int ihg2_arm_device_write_brightness(std::uint64_t aid, int percent) { return runtime.write_brightness(aid, percent) ? 1 : 0; }
extern "C" std::uint32_t ihg2_arm_device_read_color_temperature(std::uint64_t aid) { return runtime.read_color_temperature(aid); }
extern "C" int ihg2_arm_device_write_color_temperature(std::uint64_t aid, std::uint32_t mirek) { return runtime.write_color_temperature(aid, mirek) ? 1 : 0; }
extern "C" int ihg2_arm_device_read_cover_current_position(std::uint64_t aid) { return runtime.read_cover_current_position(aid); }
extern "C" int ihg2_arm_device_read_cover_target_position(std::uint64_t aid) { return runtime.read_cover_target_position(aid); }
extern "C" int ihg2_arm_device_read_cover_position_state(std::uint64_t aid) { return runtime.read_cover_position_state(aid); }
extern "C" int ihg2_arm_device_write_cover_target_position(std::uint64_t aid, int percent) { return runtime.write_cover_target_position(aid, percent) ? 1 : 0; }
extern "C" std::uint8_t ihg2_arm_device_read_thermostat_current_state(std::uint64_t aid) { return runtime.read_thermostat_current_state(aid); }
extern "C" std::uint8_t ihg2_arm_device_read_thermostat_target_state(std::uint64_t aid) { return runtime.read_thermostat_target_state(aid); }
extern "C" int ihg2_arm_device_write_thermostat_target_state(std::uint64_t aid, std::uint8_t state) { return runtime.write_thermostat_target_state(aid, state) ? 1 : 0; }
extern "C" float ihg2_arm_device_read_current_temperature(std::uint64_t aid) { return runtime.read_current_temperature(aid); }
extern "C" float ihg2_arm_device_read_target_temperature(std::uint64_t aid) { return runtime.read_target_temperature(aid); }
extern "C" int ihg2_arm_device_write_target_temperature(std::uint64_t aid, float temperature) { return runtime.write_target_temperature(aid, temperature) ? 1 : 0; }
extern "C" std::uint8_t ihg2_arm_device_read_temperature_display_units(std::uint64_t aid) { return runtime.read_temperature_display_units(aid); }
extern "C" int ihg2_arm_device_write_temperature_display_units(std::uint64_t aid, std::uint8_t units) { return runtime.write_temperature_display_units(aid, units) ? 1 : 0; }
