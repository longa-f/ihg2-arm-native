#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {
int ihg2_arm_initialize(const char* config_directory);
void ihg2_arm_poll(void);
std::size_t ihg2_arm_device_count(void);
std::uint64_t ihg2_arm_device_aid(std::size_t index);
const char* ihg2_arm_device_name(std::size_t index);
const char* ihg2_arm_homekit_name(void);
std::uint16_t ihg2_arm_homekit_port(void);
int ihg2_arm_device_is_light(std::size_t index);
int ihg2_arm_device_is_cover(std::size_t index);
int ihg2_arm_device_is_climate(std::size_t index);
int ihg2_arm_device_can_write(std::size_t index);
int ihg2_arm_device_has_brightness(std::size_t index);
int ihg2_arm_device_has_color_temperature(std::size_t index);
int ihg2_arm_device_has_cover_position(std::size_t index);
int ihg2_arm_device_read_on(std::uint64_t aid);
int ihg2_arm_device_write_on(std::uint64_t aid, int on);
int ihg2_arm_device_read_brightness(std::uint64_t aid);
int ihg2_arm_device_write_brightness(std::uint64_t aid, int percent);
std::uint32_t ihg2_arm_device_read_color_temperature(std::uint64_t aid);
int ihg2_arm_device_write_color_temperature(std::uint64_t aid, std::uint32_t mirek);
int ihg2_arm_device_read_cover_current_position(std::uint64_t aid);
int ihg2_arm_device_read_cover_target_position(std::uint64_t aid);
int ihg2_arm_device_read_cover_position_state(std::uint64_t aid);
int ihg2_arm_device_write_cover_target_position(std::uint64_t aid, int percent);
std::uint8_t ihg2_arm_device_read_thermostat_current_state(std::uint64_t aid);
std::uint8_t ihg2_arm_device_read_thermostat_target_state(std::uint64_t aid);
int ihg2_arm_device_write_thermostat_target_state(std::uint64_t aid, std::uint8_t state);
float ihg2_arm_device_read_current_temperature(std::uint64_t aid);
float ihg2_arm_device_read_target_temperature(std::uint64_t aid);
int ihg2_arm_device_write_target_temperature(std::uint64_t aid, float temperature);
std::uint8_t ihg2_arm_device_read_temperature_display_units(std::uint64_t aid);
int ihg2_arm_device_write_temperature_display_units(std::uint64_t aid, std::uint8_t units);
void ihg2_hap_state_changed(std::size_t index, std::uint32_t changed_mask);
}
