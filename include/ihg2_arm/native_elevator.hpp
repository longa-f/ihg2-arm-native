#pragma once

#include <string>
#include <vector>

namespace ihg2::arm::native_elevator {

enum class Direction { up, down };

// Returns the fixed argv passed to Android's activity manager. Kept separate
// so the direction mapping is tested without starting the elevator UI.
std::vector<std::string> activity_arguments(Direction direction);

// True when the panel has the Leelen phone app and Android activity manager.
bool available() noexcept;

// Launches the vendor elevator activity with the requested direction. The
// activity performs the call through the vendor app's existing local path.
bool launch(Direction direction) noexcept;

} // namespace ihg2::arm::native_elevator
