#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ihg2::arm::presence {

using MacAddress = std::array<std::uint8_t, 6>;

struct Config {
    std::uint32_t ipv4{}; // Network byte order.
    MacAddress mac{};
    bool operator==(const Config&) const = default;
};

[[nodiscard]] std::optional<MacAddress> parse_mac(std::string_view text) noexcept;
[[nodiscard]] std::string format_mac(const MacAddress& mac);
[[nodiscard]] std::optional<Config> make_config(std::string_view ipv4, std::string_view mac) noexcept;
[[nodiscard]] std::optional<Config> load_config(const std::string& directory);
[[nodiscard]] bool save_config(const std::string& directory, std::string_view ipv4,
                              std::string_view mac) noexcept;
[[nodiscard]] bool probe(const Config& config);

class Detector final {
public:
    [[nodiscard]] bool observe(bool matched, std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool expire(std::chrono::steady_clock::time_point now,
                              std::chrono::seconds timeout) noexcept;
    [[nodiscard]] bool online() const noexcept { return online_; }

private:
    std::chrono::steady_clock::time_point last_seen_{};
    bool online_{};
};

} // namespace ihg2::arm::presence
