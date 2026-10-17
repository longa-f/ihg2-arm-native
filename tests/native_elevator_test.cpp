#include "ihg2_arm/native_elevator.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void check_arguments(ihg2::arm::native_elevator::Direction direction,
                     const std::string& expected_direction) {
    const std::vector<std::string> expected = {
            "am", "start", "-n",
            "com.leelen.phone/com.leelen.calllift.activity.CallLiftActivity",
            "-a", "android.intent.action.MAIN", "-f", "0x10000000",
            "--es", "call_lift", expected_direction,
            "--ei", "close_time", "2"};
    require(ihg2::arm::native_elevator::activity_arguments(direction) == expected,
            "native elevator intent arguments changed");
}

} // namespace

int main() {
    check_arguments(ihg2::arm::native_elevator::Direction::up, "up");
    check_arguments(ihg2::arm::native_elevator::Direction::down, "down");
    std::cout << "native elevator intent mapping tests passed\n";
    return 0;
}
