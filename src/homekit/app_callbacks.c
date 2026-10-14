#include "HAP.h"
#include "Applications/Lightbulb/App.h"
#include "ihg2_arm/runtime_c.h"

#include <stdio.h>

HAPError IdentifyAccessory(HAPAccessoryServerRef* server HAP_UNUSED,
                           const HAPAccessoryIdentifyRequest* request HAP_UNUSED,
                           void* context HAP_UNUSED) {
    return kHAPError_None;
}

HAPError HandleLightBulbOnRead(HAPAccessoryServerRef* server HAP_UNUSED,
                               const HAPBoolCharacteristicReadRequest* request,
                               bool* value,
                               void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_on(request->accessory->aid) != 0;
    return kHAPError_None;
}

HAPError HandleLightBulbOnWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                                const HAPBoolCharacteristicWriteRequest* request,
                                bool value,
                                void* context HAP_UNUSED) {
    return ihg2_arm_device_write_on(request->accessory->aid, value) ? kHAPError_None : kHAPError_OutOfResources;
}

HAPError HandleBrightnessRead(HAPAccessoryServerRef* server HAP_UNUSED,
                              const HAPIntCharacteristicReadRequest* request,
                              int32_t* value,
                              void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_brightness(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleBrightnessWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                               const HAPIntCharacteristicWriteRequest* request,
                               int32_t value,
                               void* context HAP_UNUSED) {
    return ihg2_arm_device_write_brightness(request->accessory->aid, value) ? kHAPError_None : kHAPError_OutOfResources;
}

HAPError HandleColorTemperatureRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                    const HAPUInt32CharacteristicReadRequest* request,
                                    uint32_t* value,
                                    void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_color_temperature(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleColorTemperatureWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                                     const HAPUInt32CharacteristicWriteRequest* request,
                                     uint32_t value,
                                     void* context HAP_UNUSED) {
    return ihg2_arm_device_write_color_temperature(request->accessory->aid, value) ? kHAPError_None : kHAPError_OutOfResources;
}

HAPError HandleCoverCurrentPositionRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                        const HAPUInt8CharacteristicReadRequest* request,
                                        uint8_t* value,
                                        void* context HAP_UNUSED) {
    *value = (uint8_t) ihg2_arm_device_read_cover_current_position(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleCoverTargetPositionRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                       const HAPUInt8CharacteristicReadRequest* request,
                                       uint8_t* value,
                                       void* context HAP_UNUSED) {
    *value = (uint8_t) ihg2_arm_device_read_cover_target_position(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleCoverTargetPositionWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                                        const HAPUInt8CharacteristicWriteRequest* request,
                                        uint8_t value,
                                        void* context HAP_UNUSED) {
    const bool accepted = ihg2_arm_device_write_cover_target_position(request->accessory->aid, value) != 0;
    fprintf(stderr, "ihg2-arm: HomeKit cover write name=%s target=%u accepted=%u\n",
            request->accessory->name, (unsigned) value, accepted ? 1U : 0U);
    return accepted ? kHAPError_None : kHAPError_OutOfResources;
}

HAPError HandleCoverPositionStateRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                      const HAPUInt8CharacteristicReadRequest* request,
                                      uint8_t* value,
                                      void* context HAP_UNUSED) {
    *value = (uint8_t) ihg2_arm_device_read_cover_position_state(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleThermostatCurrentStateRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                          const HAPUInt8CharacteristicReadRequest* request,
                                          uint8_t* value,
                                          void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_thermostat_current_state(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleThermostatTargetStateRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                         const HAPUInt8CharacteristicReadRequest* request,
                                         uint8_t* value,
                                         void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_thermostat_target_state(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleThermostatTargetStateWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                                          const HAPUInt8CharacteristicWriteRequest* request,
                                          uint8_t value,
                                          void* context HAP_UNUSED) {
    return ihg2_arm_device_write_thermostat_target_state(request->accessory->aid, value)
            ? kHAPError_None : kHAPError_OutOfResources;
}

HAPError HandleCurrentTemperatureRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                      const HAPFloatCharacteristicReadRequest* request,
                                      float* value,
                                      void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_current_temperature(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleTargetTemperatureRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                     const HAPFloatCharacteristicReadRequest* request,
                                     float* value,
                                     void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_target_temperature(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleTargetTemperatureWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                                      const HAPFloatCharacteristicWriteRequest* request,
                                      float value,
                                      void* context HAP_UNUSED) {
    return ihg2_arm_device_write_target_temperature(request->accessory->aid, value)
            ? kHAPError_None : kHAPError_OutOfResources;
}

HAPError HandleTemperatureDisplayUnitsRead(HAPAccessoryServerRef* server HAP_UNUSED,
                                           const HAPUInt8CharacteristicReadRequest* request,
                                           uint8_t* value,
                                           void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_temperature_display_units(request->accessory->aid);
    return kHAPError_None;
}

HAPError HandleTemperatureDisplayUnitsWrite(HAPAccessoryServerRef* server HAP_UNUSED,
                                            const HAPUInt8CharacteristicWriteRequest* request,
                                            uint8_t value,
                                            void* context HAP_UNUSED) {
    return ihg2_arm_device_write_temperature_display_units(request->accessory->aid, value)
            ? kHAPError_None : kHAPError_OutOfResources;
}
