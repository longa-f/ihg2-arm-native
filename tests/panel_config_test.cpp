#include "ihg2_arm/panel_config.hpp"

#include <cassert>
#include <string>

using namespace ihg2::arm;

int main() {
    const Json subdevices = Json::array({
            {{"category", "remote_control_panel"},
             {"did", "parent-1"},
             {"sdid", "target-1"},
             {"devname", "Relay panel"},
             {"spid", "relay-model"},
             {"vgroups", Json::array({{{"category", "LIGHT"},
                                       {"param", "pwr1"},
                                       {"groupid", "channel-1"},
                                       {"name", "Hall relay"},
                                       {"loadtype", "device"},
                                       {"extend", "{\"loadid\":\"stable-relay\"}"}}})}},
            {{"category", "LIGHT"},
             {"did", "parent-2"},
             {"sdid", "target-2"},
             {"devname", "Dimmable strip"},
             {"devicetypeflag", 2},
             {"spid", "light-model"},
             {"_extend", "{\"tmallSn\":\"stable-strip\"}"}},
            {{"category", "remote_control_panel"},
             {"did", "parent-3"},
             {"sdid", "target-3"},
             {"devname", "Curtain panel"},
             {"vgroups", Json::array({{{"category", "CURTAIN"},
                                       {"param", "ignored-vendor-param"},
                                       {"groupid", "curtain-group"},
                                       {"name", "Window covering"},
                                       {"extend", "{\"loadid\":\"stable-cover\"}"}}})}},
            {{"category", "remote_control_panel"},
             {"did", "parent-4"},
             {"sdid", "target-4"},
             {"devname", "Thermostat panel"},
             {"vgroups", Json::array({{{"category", "AC"},
                                       {"param", "ignored-ac-param"},
                                       {"groupid", "ac-group"},
                                       {"name", "Climate"}}})}},
            {{"category", "SENSOR"},
             {"did", "parent-5"},
             {"sdid", "target-5"},
             {"devname", "Leak sensor"},
             {"spid", "sensor-27a70000"}},
            {{"category", "remote_control_panel"},
             {"did", "parent-6"},
             {"sdid", "target-6"},
             {"devname", "PWR3 panel"},
             {"vgroups", Json::array({{{"category", "VS"},
                                       {"param", "tp_vspwr"},
                                       {"groupid", "radar-channel"},
                                       {"name", "公卫雷达"},
                                       {"extend", "{\"loadid\":\"stable-radar\"}"}}})}},
            {{"category", "VS"},
             {"did", "parent-7"},
             {"sdid", "target-7"},
             {"devname", "水浸传感器"},
             {"spid", "water-leak-model"}},
            {{"category", "LIGHT"},
             {"did", "ignored"},
             {"sdid", "ignored"},
             {"ignoreflag", true}}});

    const auto inventory = inventory_from_panel_subdevices(subdevices);
    assert(inventory.size() == 7);
    assert(inventory[0]["unique_id"] == "switch_stable-relay");
    assert(inventory[0]["name"] == "Hall relay");
    assert(inventory[0]["target_did"] == "target-1");
    assert(inventory[0]["parent_did"] == "parent-1");
    assert(inventory[0]["control_key"] == "pwr1");
    assert(inventory[1]["platform"] == "light");
    assert(inventory[1]["control_key"] == "pwr");
    assert(inventory[1]["properties"] == Json::array({"pwr", "brightness", "colortemp"}));
    assert(inventory[2]["platform"] == "cover");
    assert(inventory[2]["control_key"] == "curtain_ctrl");
    assert(inventory[2]["properties"].size() == 3);
    assert(inventory[3]["platform"] == "climate");
    assert(inventory[3]["control_key"] == "tp_acpwr");
    assert(inventory[4]["platform"] == "binary_sensor");
    assert(inventory[4]["control_key"] == "water_detected");
    assert(inventory[4]["sensor_type"] == "leak");
    assert(inventory[5]["unique_id"] == "switch_stable-radar");
    assert(inventory[5]["platform"] == "binary_sensor");
    assert(inventory[5]["control_key"] == "radar_detected");
    assert(inventory[5]["sensor_type"] == "occupancy");
    assert(inventory[6]["platform"] == "binary_sensor");
    assert(inventory[6]["control_key"] == "water_detected");
    assert(inventory[6]["sensor_type"] == "leak");

    const std::string neighbors =
            "IP address HW type Flags HW address Mask Device\n"
            "192.168.5.26 0x1 0x2 aa:bb:cc:dd:ee:ff * eth0\n";
    assert(gateway_ipv4_from_arp(neighbors, "AA-BB-CC-DD-EE-FF") == "192.168.5.26");

    bool rejected = false;
    try {
        (void)gateway_ipv4_from_arp(neighbors, "11:22:33:44:55:66");
    } catch (const ProtocolError&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        (void)gateway_ipv4_from_arp(
                neighbors + "192.168.5.27 0x1 0x2 aa:bb:cc:dd:ee:ff * eth0\n",
                "aa:bb:cc:dd:ee:ff");
    } catch (const ProtocolError&) {
        rejected = true;
    }
    assert(rejected);
}
