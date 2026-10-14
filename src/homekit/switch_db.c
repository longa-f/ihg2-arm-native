#include "HAP.h"
#include "HAPRequestHandlers.h"
#include "ihg2_arm/runtime_c.h"

#define kSwitchServiceIID ((uint64_t) 0x0030)
#define kSwitchOnIID ((uint64_t) 0x0033)

static HAPError ReadOn(HAPAccessoryServerRef* server HAP_UNUSED,
                       const HAPBoolCharacteristicReadRequest* request,
                       bool* value,
                       void* context HAP_UNUSED) {
    *value = ihg2_arm_device_read_on(request->accessory->aid) != 0;
    return kHAPError_None;
}

static HAPError WriteOn(HAPAccessoryServerRef* server HAP_UNUSED,
                        const HAPBoolCharacteristicWriteRequest* request,
                        bool value,
                        void* context HAP_UNUSED) {
    return ihg2_arm_device_write_on(request->accessory->aid, value) ? kHAPError_None : kHAPError_OutOfResources;
}

static const HAPDataCharacteristic serviceSignature = {
    .format = kHAPCharacteristicFormat_Data,
    .iid = 0x0031,
    .characteristicType = &kHAPCharacteristicType_ServiceSignature,
    .debugDescription = kHAPCharacteristicDebugDescription_ServiceSignature,
    .manufacturerDescription = NULL,
    .properties = { .readable = true, .writable = false, .supportsEventNotification = false,
                    .hidden = false, .requiresTimedWrite = false, .supportsAuthorizationData = false,
                    .ip = { .controlPoint = true },
                    .ble = { .supportsBroadcastNotification = false, .supportsDisconnectedNotification = false,
                             .readableWithoutSecurity = false, .writableWithoutSecurity = false } },
    .constraints = { .maxLength = 2097152 },
    .callbacks = { .handleRead = HAPHandleServiceSignatureRead, .handleWrite = NULL }
};

static const HAPBoolCharacteristic onCharacteristic = {
    .format = kHAPCharacteristicFormat_Bool,
    .iid = kSwitchOnIID,
    .characteristicType = &kHAPCharacteristicType_On,
    .debugDescription = kHAPCharacteristicDebugDescription_On,
    .manufacturerDescription = NULL,
    .properties = { .readable = true, .writable = true, .supportsEventNotification = true,
                    .hidden = false, .requiresTimedWrite = false, .supportsAuthorizationData = false,
                    .ip = { .controlPoint = false, .supportsWriteResponse = false },
                    .ble = { .supportsBroadcastNotification = false, .supportsDisconnectedNotification = false,
                             .readableWithoutSecurity = false, .writableWithoutSecurity = false } },
    .callbacks = { .handleRead = ReadOn, .handleWrite = WriteOn }
};

const HAPService ihg2SwitchService = {
    .iid = kSwitchServiceIID,
    .serviceType = &kHAPServiceType_Switch,
    .debugDescription = kHAPServiceDebugDescription_Switch,
    .name = "Switch",
    .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
    .linkedServices = NULL,
    .characteristics = (const HAPCharacteristic* const[]) { &serviceSignature, &onCharacteristic, NULL }
};
