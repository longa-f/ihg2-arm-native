#pragma once

#include <cstdint>
#include <optional>

namespace ihg2::arm {

// HomeKit PositionState: 0 = closing, 1 = opening, 2 = stopped.
constexpr std::optional<std::uint8_t> homekit_cover_state_from_gateway(int value) {
    switch (value) {
        case 0: return 2; // Gateway reports stopped as 0.
        case 1: return 1; // Captured while the curtain was opening.
        case 2: return 0; // Gateway's opposite direction state.
        default: return std::nullopt;
    }
}

constexpr int homekit_cover_target_from_motion(int current, std::uint8_t state) {
    if (state == 1) return 100;
    if (state == 0) return 0;
    return current;
}

constexpr std::uint8_t homekit_cover_state_for_target(int current, int target) {
    if (target > current) return 1; // Opening.
    if (target < current) return 0; // Closing.
    return 2; // Stopped at the requested position.
}

} // namespace ihg2::arm
