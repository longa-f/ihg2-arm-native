#pragma once

#include "ihg2_arm/vendor_protocol/protocol.hpp"

#include <string>
#include <string_view>

namespace ihg2::arm {

struct PanelSdkConfig {
    PanelSdkConfig() = default;
    PanelSdkConfig(const PanelSdkConfig&) = delete;
    PanelSdkConfig& operator=(const PanelSdkConfig&) = delete;
    PanelSdkConfig(PanelSdkConfig&& other) noexcept;
    PanelSdkConfig& operator=(PanelSdkConfig&& other) noexcept;
    ~PanelSdkConfig();

    Mac gateway_mac{};
    Key gateway_key{};
    Json inventory;
};

// Convert the vendor SDK's subdevs.db JSON into the bridge's stable accessory inventory.
Json inventory_from_panel_subdevices(const Json& subdevices);

// Resolve an IPv4 neighbor by MAC from the Android /proc/net/arp table.
std::string gateway_ipv4_from_arp(std::string_view arp_table, const std::string& mac);
std::string gateway_ipv4_from_arp_file(
        const Mac& mac, const std::string& arp_path = "/proc/net/arp");

// Read only the vendor SDK's local IHG2 identity, key and device list. Does not write files.
PanelSdkConfig load_panel_sdk_config(
        const std::string& config_path =
                "/data/user/0/cn.net.cloudthink.smartmirror/app_gatewaysdk/config.db",
        const std::string& subdevices_path =
                "/data/user/0/cn.net.cloudthink.smartmirror/app_gatewaysdk/subdevs.db");

} // namespace ihg2::arm
