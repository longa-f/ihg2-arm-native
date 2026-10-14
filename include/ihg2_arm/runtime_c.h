#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int ihg2_arm_initialize(const char* config_directory);
int ihg2_arm_panel_adoption_probe(size_t* accessory_count);
int ihg2_arm_doctor(const char* config_directory);
void ihg2_arm_poll(void);
size_t ihg2_arm_device_count(void);
uint64_t ihg2_arm_device_aid(size_t index);
const char* ihg2_arm_device_name(size_t index);
const char* ihg2_arm_homekit_name(void);
uint16_t ihg2_arm_homekit_port(void);
int ihg2_arm_device_is_light(size_t index);
int ihg2_arm_device_is_cover(size_t index);
int ihg2_arm_device_is_climate(size_t index);
int ihg2_arm_device_can_write(size_t index);
int ihg2_arm_device_has_brightness(size_t index);
int ihg2_arm_device_has_color_temperature(size_t index);
int ihg2_arm_device_has_cover_position(size_t index);
int ihg2_arm_device_read_on(uint64_t aid);
int ihg2_arm_device_write_on(uint64_t aid, int on);
int ihg2_arm_device_read_brightness(uint64_t aid);
int ihg2_arm_device_write_brightness(uint64_t aid, int percent);
uint32_t ihg2_arm_device_read_color_temperature(uint64_t aid);
int ihg2_arm_device_write_color_temperature(uint64_t aid, uint32_t mirek);
int ihg2_arm_device_read_cover_current_position(uint64_t aid);
int ihg2_arm_device_read_cover_target_position(uint64_t aid);
int ihg2_arm_device_read_cover_position_state(uint64_t aid);
int ihg2_arm_device_write_cover_target_position(uint64_t aid, int percent);
uint8_t ihg2_arm_device_read_thermostat_current_state(uint64_t aid);
uint8_t ihg2_arm_device_read_thermostat_target_state(uint64_t aid);
int ihg2_arm_device_write_thermostat_target_state(uint64_t aid, uint8_t state);
float ihg2_arm_device_read_current_temperature(uint64_t aid);
float ihg2_arm_device_read_target_temperature(uint64_t aid);
int ihg2_arm_device_write_target_temperature(uint64_t aid, float temperature);
uint8_t ihg2_arm_device_read_temperature_display_units(uint64_t aid);
int ihg2_arm_device_write_temperature_display_units(uint64_t aid, uint8_t units);

enum {
    IHG2_HAP_STATE_ON = 1u << 0,
    IHG2_HAP_STATE_BRIGHTNESS = 1u << 1,
    IHG2_HAP_STATE_COLOR_TEMPERATURE = 1u << 2,
    IHG2_HAP_STATE_COVER_CURRENT = 1u << 3,
    IHG2_HAP_STATE_COVER_TARGET = 1u << 4,
    IHG2_HAP_STATE_COVER_POSITION_STATE = 1u << 5,
    IHG2_HAP_STATE_THERMOSTAT_CURRENT_STATE = 1u << 6,
    IHG2_HAP_STATE_THERMOSTAT_TARGET_STATE = 1u << 7,
    IHG2_HAP_STATE_CURRENT_TEMPERATURE = 1u << 8,
    IHG2_HAP_STATE_TARGET_TEMPERATURE = 1u << 9
};
void ihg2_hap_state_changed(size_t index, uint32_t changed_mask);
#ifdef __cplusplus
}
#endif
