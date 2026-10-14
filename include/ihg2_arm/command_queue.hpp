#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ihg2::arm {

struct PowerCommand {
    std::uint16_t device_index{0};
    bool on{false};
    std::uint32_t sequence{0};
};

template <std::size_t Capacity>
class CommandQueue final {
    static_assert(Capacity > 0, "queue capacity must be nonzero");

public:
    [[nodiscard]] bool push(const PowerCommand& command) noexcept {
        if (size_ == Capacity) {
            return false;
        }
        entries_[tail_] = command;
        tail_ = (tail_ + 1) % Capacity;
        ++size_;
        return true;
    }

    [[nodiscard]] bool pop(PowerCommand& command) noexcept {
        if (size_ == 0) {
            return false;
        }
        command = entries_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

private:
    std::array<PowerCommand, Capacity> entries_{};
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
};

} // namespace ihg2::arm
