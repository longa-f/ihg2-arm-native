#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace ihg2::arm {

enum class AccessoryKind : std::uint8_t {
    light,
    switch_device,
};

template <std::size_t Capacity>
class FixedText final {
public:
    FixedText() noexcept = default;

    explicit FixedText(std::string_view value) {
        if (value.empty() || value.size() > Capacity || value.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("invalid fixed text length or content");
        }
        std::memcpy(bytes_.data(), value.data(), value.size());
        size_ = value.size();
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return {bytes_.data(), size_};
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool operator==(std::string_view other) const noexcept {
        return view() == other;
    }

private:
    std::array<char, Capacity + 1> bytes_{};
    std::size_t size_{0};
};

struct DeviceSpec {
    FixedText<96> id;
    FixedText<63> name;
    AccessoryKind kind{AccessoryKind::light};

    DeviceSpec(std::string_view device_id, std::string_view device_name, AccessoryKind device_kind)
        : id(device_id), name(device_name), kind(device_kind) {}
};

struct DeviceState {
    bool on{false};
    bool known{false};
    std::uint64_t revision{0};
    std::uint64_t last_report_ms{0};
};

struct StateChange {
    std::size_t index{0};
    bool on{false};
    std::uint64_t revision{0};
};

class StateStore final {
public:
    explicit StateStore(std::vector<DeviceSpec> devices);

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const DeviceSpec& spec(std::size_t index) const;
    [[nodiscard]] DeviceState state(std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> find(std::string_view id) const noexcept;

    // Returns no value for an unknown device or an unchanged report.
    [[nodiscard]] std::optional<StateChange> apply_power_report(
        std::string_view id, bool on, std::uint64_t monotonic_ms) noexcept;

private:
    std::vector<DeviceSpec> devices_;
    std::vector<DeviceState> states_;
};

} // namespace ihg2::arm
