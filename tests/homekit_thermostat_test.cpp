#include "ihg2_arm/homekit_thermostat.hpp"

#include <cassert>

using namespace ihg2::arm;

int main() {
    assert(homekit_target_state_from_gateway_mode(0) == 3);
    assert(homekit_target_state_from_gateway_mode(1) == 2);
    assert(homekit_target_state_from_gateway_mode(2) == 3);
    assert(homekit_target_state_from_gateway_mode(3) == 3);
    assert(homekit_target_state_from_gateway_mode(4) == 1);
    assert(!homekit_target_state_from_gateway_mode(5));
    assert(gateway_mode_from_homekit_target_state(1) == 4);
    assert(gateway_mode_from_homekit_target_state(2) == 1);
    assert(gateway_mode_from_homekit_target_state(3) == 0);
    assert(!gateway_mode_from_homekit_target_state(0));
    assert(homekit_current_state_from_gateway_mode(false, 1, 20.0F, 24.0F) == 0);
    assert(homekit_current_state_from_gateway_mode(true, 4, 20.0F, 24.0F) == 1);
    assert(homekit_current_state_from_gateway_mode(true, 1, 28.0F, 24.0F) == 2);
    assert(homekit_current_state_from_gateway_mode(true, 0, 28.0F, 24.0F) == 2);
    assert(homekit_current_state_from_gateway_mode(true, 0, 20.0F, 24.0F) == 1);
    assert(homekit_current_state_from_gateway_mode(true, 0, 24.0F, 24.0F) == 0);
    assert(homekit_current_state_from_gateway_mode(true, 2, 28.0F, 24.0F) == 0);
    assert(homekit_current_state_from_gateway_mode(true, 3, 20.0F, 24.0F) == 0);
}
