#include "ihg2_arm/homekit_cover.hpp"

#include <cassert>

using namespace ihg2::arm;

int main() {
    const auto stopped = homekit_cover_state_from_gateway(0);
    const auto opening = homekit_cover_state_from_gateway(1);
    const auto closing = homekit_cover_state_from_gateway(2);
    assert(stopped && *stopped == 2);
    assert(opening && *opening == 1);
    assert(closing && *closing == 0);
    assert(!homekit_cover_state_from_gateway(-1));
    assert(homekit_cover_target_from_motion(48, 2) == 48);
    assert(homekit_cover_target_from_motion(48, 1) == 100);
    assert(homekit_cover_target_from_motion(48, 0) == 0);
    assert(homekit_cover_state_for_target(0, 50) == 1);
    assert(homekit_cover_state_for_target(50, 0) == 0);
    assert(homekit_cover_state_for_target(50, 50) == 2);
}
