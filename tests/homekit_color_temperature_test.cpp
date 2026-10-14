#include "ihg2_arm/homekit_color_temperature.hpp"

#include <cassert>

using namespace ihg2::arm;

int main() {
    assert(homekit_mirek_from_gateway_colortemp(0) == 400);
    assert(homekit_mirek_from_gateway_colortemp(6) == 379);
    assert(homekit_mirek_from_gateway_colortemp(50) == 225);
    assert(homekit_mirek_from_gateway_colortemp(100) == 50);
    assert(!homekit_mirek_from_gateway_colortemp(-1));
    assert(!homekit_mirek_from_gateway_colortemp(101));
    assert(gateway_colortemp_from_homekit_mirek(50) == 100);
    assert(gateway_colortemp_from_homekit_mirek(225) == 50);
    assert(gateway_colortemp_from_homekit_mirek(400) == 0);
    assert(!gateway_colortemp_from_homekit_mirek(49));
    assert(!gateway_colortemp_from_homekit_mirek(401));
}
