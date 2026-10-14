#include "ihg2_arm/state_store.hpp"

#include <stdexcept>

namespace ihg2::arm {

StateStore::StateStore(std::vector<DeviceSpec> devices)
    : devices_(std::move(devices)), states_(devices_.size()) {
    if (devices_.empty() || devices_.size() > 128) {
        throw std::invalid_argument("device catalog must contain 1..128 entries");
    }

    for (std::size_t i = 0; i < devices_.size(); ++i) {
        const auto& device = devices_[i];
        for (std::size_t j = 0; j < i; ++j) {
            if (devices_[j].id.view() == device.id.view()) {
                throw std::invalid_argument("duplicate device id");
            }
        }
    }
}

std::size_t StateStore::size() const noexcept {
    return devices_.size();
}

const DeviceSpec& StateStore::spec(std::size_t index) const {
    return devices_.at(index);
}

DeviceState StateStore::state(std::size_t index) const {
    return states_.at(index);
}

std::optional<std::size_t> StateStore::find(std::string_view id) const noexcept {
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].id.view() == id) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<StateChange> StateStore::apply_power_report(
    std::string_view id, bool on, std::uint64_t monotonic_ms) noexcept {
    const auto index = find(id);
    if (!index) {
        return std::nullopt;
    }

    auto& current = states_[*index];
    if (current.known && monotonic_ms < current.last_report_ms) {
        return std::nullopt;
    }
    current.last_report_ms = monotonic_ms;
    if (current.known && current.on == on) {
        return std::nullopt;
    }

    current.on = on;
    current.known = true;
    ++current.revision;
    return StateChange{*index, on, current.revision};
}

} // namespace ihg2::arm
