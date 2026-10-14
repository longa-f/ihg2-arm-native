#pragma once

#include <cstdint>
#include <optional>

namespace ihg2::arm {

// Provisional gateway AC mode map: 0 auto, 1 cool, 2 dry, 3 fan, 4 heat.
// HomeKit has no dry or fan target state, so those modes are represented as
// Auto while their power and temperature remain readable.
constexpr std::optional<std::uint8_t> homekit_target_state_from_gateway_mode(int mode) {
    switch (mode) {
        case 0: return 3; // Auto.
        case 1: return 2; // Cool.
        case 2: return 3; // Dry is outside the HomeKit thermostat mode set.
        case 3: return 3; // Fan-only is outside the HomeKit thermostat mode set.
        case 4: return 1; // Heat.
        default: return std::nullopt;
    }
}

constexpr std::optional<int> gateway_mode_from_homekit_target_state(std::uint8_t state) {
    switch (state) {
        case 1: return 4; // Heat.
        case 2: return 1; // Cool.
        case 3: return 0; // Auto.
        default: return std::nullopt;
    }
}

constexpr std::uint8_t homekit_current_state_from_gateway_mode(
        bool powered, int gateway_mode, float current_temperature, float target_temperature) {
    if (!powered) return 0;
    if (gateway_mode == 4) return 1;
    if (gateway_mode == 1) return 2;
    if (gateway_mode == 0) {
        if (current_temperature > target_temperature) return 2;
        if (current_temperature < target_temperature) return 1;
    }
    return 0;
}

} // namespace ihg2::arm
