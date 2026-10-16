#include "ihg2_arm/panel_config.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ihg2::arm {
namespace {

void wipe(std::string& value) noexcept {
    volatile char* p = value.empty() ? nullptr : value.data();
    for (std::size_t i = 0; i < value.size(); ++i) p[i] = '\0';
    value.clear();
}

void wipe(Key& value) noexcept {
    volatile std::uint8_t* p = value.data();
    for (std::size_t i = 0; i < value.size(); ++i) p[i] = 0;
}

void wipe_json(Json& value) noexcept {
    if (value.is_string()) {
        auto& text = value.get_ref<std::string&>();
        wipe(text);
    } else if (value.is_array()) {
        for (auto& item : value) wipe_json(item);
    } else if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) wipe_json(it.value());
    }
}

struct JsonWiper {
    Json& value;
    ~JsonWiper() { wipe_json(value); }
};

std::string read_private_sdk_file(const std::string& path, std::size_t limit) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw ProtocolError("vendor SDK database is unavailable");
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0077) != 0 ||
        st.st_size <= 0 || static_cast<std::uint64_t>(st.st_size) > limit) {
        ::close(fd);
        throw ProtocolError("vendor SDK database permissions or size are invalid");
    }
    std::string contents(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const auto n = ::read(fd, contents.data() + offset, contents.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ::close(fd);
            wipe(contents);
            throw ProtocolError("vendor SDK database read failed");
        }
        offset += static_cast<std::size_t>(n);
    }
    char extra{};
    const auto more = ::read(fd, &extra, 1);
    ::close(fd);
    if (more != 0) {
        wipe(contents);
        throw ProtocolError("vendor SDK database changed while reading");
    }
    return contents;
}

Json read_sdk_json(const std::string& path, std::size_t limit) {
    auto contents = read_private_sdk_file(path, limit);
    Json value;
    try {
        value = bounded_json(contents, limit, 32768);
    } catch (...) {
        wipe(contents);
        throw ProtocolError("vendor SDK database JSON is invalid");
    }
    wipe(contents);
    return value;
}

std::string read_proc_table(const std::string& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw ProtocolError("panel ARP neighbor table is unavailable");
    std::string contents;
    contents.reserve(1024);
    char buffer[512];
    while (input.read(buffer, sizeof buffer) || input.gcount() > 0) {
        const auto count = static_cast<std::size_t>(input.gcount());
        if (count > limit - contents.size()) throw ProtocolError("panel ARP neighbor table exceeds its size limit");
        contents.append(buffer, count);
    }
    if (input.bad()) throw ProtocolError("panel ARP neighbor table read failed");
    return contents;
}

std::string text(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer() || value.is_number_unsigned()) return value.dump();
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    return {};
}

bool truthy(const Json& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value != 0;
    if (value.is_string()) return !value.get_ref<const std::string&>().empty();
    if (value.is_array() || value.is_object()) return !value.empty();
    return false;
}

bool integer_is(const Json& value, int expected) {
    if (value.is_number_integer()) return value.get<std::int64_t>() == expected;
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() == static_cast<std::uint64_t>(expected);
    return value.is_string() && value.get_ref<const std::string&>() == std::to_string(expected);
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool named_sensor(const std::string& name, const std::string& spid) {
    const auto searchable_name = upper(name);
    const auto searchable_spid = upper(spid);
    return searchable_name.find("水浸") != std::string::npos ||
           searchable_name.find("漏水") != std::string::npos ||
           searchable_name.find("WATER") != std::string::npos ||
           searchable_name.find("LEAK") != std::string::npos ||
           searchable_name.find("雷达") != std::string::npos ||
           searchable_name.find("人体") != std::string::npos ||
           searchable_name.find("RADAR") != std::string::npos ||
           searchable_name.find("MOTION") != std::string::npos ||
           searchable_name.find("OCCUPANCY") != std::string::npos ||
           (searchable_spid.size() >= 8 && searchable_spid.ends_with("27A70000"));
}

Json object_value(const Json& value) {
    if (value.is_object()) return value;
    if (value.is_string()) {
        try {
            auto parsed = Json::parse(value.get_ref<const std::string&>());
            return parsed.is_object() ? parsed : Json::object();
        } catch (const Json::exception&) {
            return Json::object();
        }
    }
    return Json::object();
}

std::vector<std::string> properties_for(const std::string& platform, const std::string& key) {
    if (platform == "light") return {key, "brightness", "colortemp"};
    if (platform == "cover") return {key, "curtain_screentargetpos", "motostatus"};
    if (platform == "climate") return {key, "ac_mode", "ac_temp", "ac_mark", "insidetemp"};
    if (platform == "binary_sensor") return {key, "battery", "batterylevel", "seq_sensor", "status"};
    return {key};
}

void append_properties(Json& item, const std::vector<std::string>& properties) {
    item["properties"] = Json::array();
    for (const auto& property : properties) item["properties"].push_back(property);
}

std::string normalized_mac(const std::string& value) {
    std::string compact;
    compact.reserve(12);
    for (const char c : value) {
        if (c == ':' || c == '-') continue;
        compact.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (compact.size() != 12 || !std::all_of(compact.begin(), compact.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        }))
        throw ProtocolError("invalid gateway MAC in vendor SDK database");
    return compact;
}

Json parse_extend(const Json& value) {
    return object_value(value);
}

} // namespace

PanelSdkConfig::PanelSdkConfig(PanelSdkConfig&& other) noexcept
    : gateway_mac(other.gateway_mac), gateway_key(other.gateway_key),
      inventory(std::move(other.inventory)) {
    wipe(other.gateway_key);
}

PanelSdkConfig& PanelSdkConfig::operator=(PanelSdkConfig&& other) noexcept {
    if (this == &other) return *this;
    wipe(gateway_key);
    gateway_mac = other.gateway_mac;
    gateway_key = other.gateway_key;
    inventory = std::move(other.inventory);
    wipe(other.gateway_key);
    return *this;
}

PanelSdkConfig::~PanelSdkConfig() {
    wipe(gateway_key);
}

Json inventory_from_panel_subdevices(const Json& subdevices) {
    if (!subdevices.is_array()) throw ProtocolError("vendor SDK device list is not an array");

    Json inventory = Json::array();
    std::set<std::string> seen;
    for (const auto& raw : subdevices) {
        if (!raw.is_object()) continue;

        std::vector<const Json*> groups;
        if (raw.contains("vgroups") && raw["vgroups"].is_array()) {
            for (const auto& group : raw["vgroups"]) {
                if (!group.is_object() || truthy(group.value("ignoreflag", Json{})) ||
                    truthy(group.value("emptyconfig", Json{})))
                    continue;
                const auto loadtype = group.contains("loadtype") ? text(group["loadtype"]) : "device";
                if (loadtype != "device") continue;
                groups.push_back(&group);
            }
        }

        const auto raw_category = upper(text(raw.value("category", Json{})));
        const auto raw_name = text(raw.value("devname", Json{}));
        const auto raw_spid = text(raw.value("spid", Json{}));
        const bool raw_fallback =
                (raw_category == "LIGHT" || raw_category == "SENSOR" ||
                 (raw_category == "VS" && named_sensor(raw_name, raw_spid))) &&
                !truthy(raw.value("ignoreflag", Json{}));
        const bool use_raw = groups.empty() && raw_fallback;
        if (groups.empty() && !use_raw) continue;

        const std::size_t candidate_count = use_raw ? 1 : groups.size();
        for (std::size_t group_index = 0; group_index < candidate_count; ++group_index) {
            const Json* group = use_raw ? nullptr : groups[group_index];
            const Json empty = Json::object();
            const Json& g = group ? *group : empty;
            auto category = upper(text(g.value("category", Json{})));
            if (category.empty()) category = upper(text(raw.value("category", Json{})));

            std::string target = text(raw.value("sdid", Json{}));
            if (target.empty()) target = text(raw.value("did", Json{}));
            if (target.empty()) continue;
            auto parent = text(raw.value("did", Json{}));
            if (parent.empty()) parent = target;

            if (category != "LIGHT" && category != "CURTAIN" && category != "AC" &&
                category != "VS" && category != "SENSOR")
                continue;

            const auto device_type = raw.value("devicetypeflag", Json{});
            const auto spid = text(raw.value("spid", Json{}));
            const auto device_name = text(raw.value("devname", Json{}));
            const auto group_name = text(g.value("name", Json{}));
            const auto searchable_name = upper(device_name + " " + group_name);
            const auto searchable_spid = upper(spid);
            const bool water_sensor = searchable_name.find("水浸") != std::string::npos ||
                    searchable_name.find("漏水") != std::string::npos ||
                    searchable_name.find("WATER") != std::string::npos ||
                    searchable_name.find("LEAK") != std::string::npos ||
                    (searchable_spid.size() >= 8 && searchable_spid.ends_with("27A70000"));
            const bool radar_sensor = searchable_name.find("雷达") != std::string::npos ||
                    searchable_name.find("人体") != std::string::npos ||
                    searchable_name.find("RADAR") != std::string::npos ||
                    searchable_name.find("MOTION") != std::string::npos ||
                    searchable_name.find("OCCUPANCY") != std::string::npos;
            const bool binary_sensor = category == "SENSOR" ||
                    (category == "VS" && (water_sensor || radar_sensor));
            const bool dimmer = category == "LIGHT" &&
                    (integer_is(device_type, 2) ||
                     (spid.size() >= 8 && upper(spid.substr(spid.size() - 8)) == "3FAA0000") ||
                     device_name.find("调光") != std::string::npos ||
                     device_name.find("灯带") != std::string::npos);
            const auto parameter = text(g.value("param", Json{}));
            if (group && parameter.empty() && category != "SENSOR") continue;

            std::string platform;
            if (category == "CURTAIN") platform = "cover";
            else if (category == "AC") platform = "climate";
            else if (binary_sensor) platform = "binary_sensor";
            else platform = dimmer ? "light" : "switch";

            std::string control_key;
            if (category == "CURTAIN") control_key = "curtain_ctrl";
            else if (category == "AC") control_key = "tp_acpwr";
            else if (binary_sensor) control_key = water_sensor ? "water_detected" : "radar_detected";
            else if (category == "VS") control_key = "tp_vspwr";
            else control_key = parameter.empty() ? "pwr" : parameter;

            const auto group_id = text(g.value("groupid", Json{}));
            Json extend = Json::object();
            if (group) {
                extend = parse_extend(g.value("extend", Json{}));
            } else {
                extend = parse_extend(raw.value("_extend", Json{}));
                if (extend.empty()) extend = parse_extend(raw.value("extend", Json{}));
            }
            auto identity = text(extend.value("loadid", Json{}));
            if (identity.empty()) identity = text(extend.value("tmallSn", Json{}));
            if (identity.empty()) identity = group ? target + "_" + group_id + "_" + parameter : target;
            // Keep the previous VS-derived switch AID when promoting a named
            // radar or leak device to a sensor service in HomeKit.
            const auto id_platform = binary_sensor && category == "VS" ? "switch" : platform;
            const auto unique_id = id_platform + "_" + identity;
            if (!seen.insert(unique_id).second) continue;

            auto name = group_name;
            if (name.empty()) name = device_name;
            if (name.empty()) name = category;

            Json item = {
                    {"unique_id", unique_id},
                    {"name", name},
                    {"platform", platform},
                    {"category", category},
                    {"target_did", target},
                    {"parent_did", parent},
                    {"groupid", group_id},
                    {"control_key", control_key},
                    {"sequence_key", ""}};
            if (binary_sensor) item["sensor_type"] = water_sensor ? "leak" : "occupancy";
            append_properties(item, properties_for(platform, control_key));
            inventory.push_back(std::move(item));
        }
    }

    if (inventory.empty() || inventory.size() > 32)
        throw ProtocolError("vendor SDK device inventory is empty or exceeds the 32-accessory limit");
    return inventory;
}

std::string gateway_ipv4_from_arp(std::string_view arp_table, const std::string& mac) {
    const auto expected = normalized_mac(mac);
    std::istringstream input{std::string(arp_table)};
    std::string line;
    std::string found;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string ip, hw_type, flags, hardware, mask, device;
        if (!(row >> ip >> hw_type >> flags >> hardware >> mask >> device)) continue;
        if (hardware.size() != 17 && hardware.size() != 12) continue;
        std::string observed;
        try {
            observed = normalized_mac(hardware);
        } catch (const ProtocolError&) {
            continue;
        }
        if (observed != expected) continue;
        char* end = nullptr;
        errno = 0;
        const auto flag_value = std::strtoul(flags.c_str(), &end, 16);
        if (errno != 0 || end == flags.c_str() || *end != '\0' || (flag_value & 0x2UL) == 0) continue;
        in_addr address{};
        if (::inet_pton(AF_INET, ip.c_str(), &address) != 1) continue;
        if (!found.empty() && found != ip) throw ProtocolError("gateway MAC has ambiguous ARP neighbors");
        found = ip;
    }
    if (found.empty()) throw ProtocolError("gateway MAC is not present in the panel ARP neighbor table");
    return found;
}

std::string gateway_ipv4_from_arp_file(const Mac& mac, const std::string& arp_path) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string mac_text;
    mac_text.reserve(17);
    for (std::size_t i = 0; i < mac.size(); ++i) {
        if (i) mac_text.push_back(':');
        mac_text.push_back(digits[mac[i] >> 4]);
        mac_text.push_back(digits[mac[i] & 0x0f]);
    }
    auto arp = read_proc_table(arp_path, 64 * 1024);
    try {
        auto ip = gateway_ipv4_from_arp(arp, mac_text);
        wipe(arp);
        return ip;
    } catch (...) {
        wipe(arp);
        throw;
    }
}

PanelSdkConfig load_panel_sdk_config(const std::string& config_path,
                                     const std::string& subdevices_path) {
    auto config = read_sdk_json(config_path, 64 * 1024);
    JsonWiper config_wiper{config};
    if (!config.is_object() || !config.value("hasDevList", false))
        throw ProtocolError("vendor SDK has not synchronized its gateway device list");
    if (!config.contains("ihgAeskey") || !config["ihgAeskey"].is_string() ||
        !config.contains("ihgMac") || !config["ihgMac"].is_string())
        throw ProtocolError("vendor SDK gateway identity is incomplete");

    auto key_text = config["ihgAeskey"].get<std::string>();
    while (!key_text.empty() && std::isspace(static_cast<unsigned char>(key_text.back()))) key_text.pop_back();
    auto mac_text = config["ihgMac"].get<std::string>();
    PanelSdkConfig result;
    try {
        result.gateway_key = parse_key(key_text);
        result.gateway_mac = parse_mac(mac_text);
    } catch (const ProtocolError&) {
        wipe(key_text);
        wipe(mac_text);
        wipe(result.gateway_key);
        throw ProtocolError("vendor SDK gateway key or MAC has an invalid format");
    }
    wipe(key_text);
    wipe(mac_text);

    auto subdevices = read_sdk_json(subdevices_path, 1024 * 1024);
    JsonWiper subdevices_wiper{subdevices};
    result.inventory = inventory_from_panel_subdevices(subdevices);

    return result;
}

} // namespace ihg2::arm
