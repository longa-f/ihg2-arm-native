#pragma once

#include <cstdint>
#include <optional>

namespace ihg2::arm {

constexpr std::uint32_t kHomeKitMinimumMirek = 50;
constexpr std::uint32_t kHomeKitMaximumMirek = 400;

// The gateway reports a 0..100 slider. A user-confirmed warmest setting read back as 0,
// so the gateway direction is opposite to HomeKit's mirek direction (higher mirek is warmer).
constexpr std::optional<std::uint32_t> homekit_mirek_from_gateway_colortemp(int raw) {
    if (raw < 0 || raw > 100) return std::nullopt;
    return kHomeKitMaximumMirek - static_cast<std::uint32_t>((raw * 350 + 50) / 100);
}

constexpr std::optional<int> gateway_colortemp_from_homekit_mirek(std::uint32_t mirek) {
    if (mirek < kHomeKitMinimumMirek || mirek > kHomeKitMaximumMirek) return std::nullopt;
    return static_cast<int>(((kHomeKitMaximumMirek - mirek) * 100 + 175) / 350);
}

} // namespace ihg2::arm
