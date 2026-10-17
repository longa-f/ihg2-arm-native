#include "HAP.h"
#include "HAPAccessorySetup.h"
#include "HAPCrypto.h"
#include "HAPRequestHandlers.h"
#include "HAPPlatform+Init.h"
#include "HAPPlatformAccessorySetup+Init.h"
#include "HAPPlatformKeyValueStore+Init.h"
#include "HAPPlatformKeyValueStore+SDKDomains.h"
#include "HAPPlatformRunLoop+Init.h"
#include "HAPPlatformServiceDiscovery+Init.h"
#include "HAPPlatformTCPStreamManager+Init.h"
#include "HAPPlatformTimer.h"
#include "HAPPlatformRandomNumber.h"
#include "HAPPlatformClock.h"
#include "Applications/Lightbulb/DB.h"
#include "ihg2_arm/log_writer.h"
#include "ihg2_arm/runtime_c.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef IHG2_ARM_VERSION
#error "IHG2_ARM_VERSION must be set from the project VERSION file"
#endif

#define kBridgeVersionText "IHG2 ARM bridge version " IHG2_ARM_VERSION

#define kMaxDevices 32
#define kMaxExtraAccessories 3
#define kAttributeCapacity 1024
#define kSessionCount 8
// HAPTime uses milliseconds. Keep gateway receive latency low without a
// 100 Hz timer allocation, and sample the slow lock monitor once per second.
#define kGatewayPollInterval (50 * HAPMillisecond)
#define kLockSensorPollInterval HAPSecond

extern const HAPService ihg2SwitchService;
extern HAPError IdentifyAccessory(HAPAccessoryServerRef*, const HAPAccessoryIdentifyRequest*, void*);
extern HAPError HandleLightBulbOnRead(HAPAccessoryServerRef*, const HAPBoolCharacteristicReadRequest*, bool*, void*);
extern HAPError HandleLightBulbOnWrite(HAPAccessoryServerRef*, const HAPBoolCharacteristicWriteRequest*, bool, void*);
extern HAPError HandleBrightnessRead(HAPAccessoryServerRef*, const HAPIntCharacteristicReadRequest*, int32_t*, void*);
extern HAPError HandleBrightnessWrite(HAPAccessoryServerRef*, const HAPIntCharacteristicWriteRequest*, int32_t, void*);
extern HAPError HandleColorTemperatureRead(HAPAccessoryServerRef*, const HAPUInt32CharacteristicReadRequest*, uint32_t*, void*);
extern HAPError HandleColorTemperatureWrite(HAPAccessoryServerRef*, const HAPUInt32CharacteristicWriteRequest*, uint32_t, void*);
extern HAPError HandleCoverCurrentPositionRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleCoverTargetPositionRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleCoverTargetPositionWrite(HAPAccessoryServerRef*, const HAPUInt8CharacteristicWriteRequest*, uint8_t, void*);
extern HAPError HandleCoverPositionStateRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleThermostatCurrentStateRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleThermostatTargetStateRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleThermostatTargetStateWrite(HAPAccessoryServerRef*, const HAPUInt8CharacteristicWriteRequest*, uint8_t, void*);
extern HAPError HandleCurrentTemperatureRead(HAPAccessoryServerRef*, const HAPFloatCharacteristicReadRequest*, float*, void*);
extern HAPError HandleTargetTemperatureRead(HAPAccessoryServerRef*, const HAPFloatCharacteristicReadRequest*, float*, void*);
extern HAPError HandleTargetTemperatureWrite(HAPAccessoryServerRef*, const HAPFloatCharacteristicWriteRequest*, float, void*);
extern HAPError HandleTemperatureDisplayUnitsRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleTemperatureDisplayUnitsWrite(HAPAccessoryServerRef*, const HAPUInt8CharacteristicWriteRequest*, uint8_t, void*);
extern HAPError HandleLockOccupancyRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);
extern HAPError HandleDeviceSensorRead(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*);

static HAPPlatformKeyValueStore keyValueStore;
static HAPPlatformAccessorySetup accessorySetup;
static HAPPlatformTCPStreamManager tcpStreamManager;
static HAPPlatformServiceDiscovery serviceDiscovery;
static HAPPlatform hapPlatform;
static HAPAccessoryServerOptions serverOptions;
static HAPAccessoryServerCallbacks serverCallbacks;
static HAPAccessoryServerRef accessoryServer;
static HAPIPSession sessions[kSessionCount];
static uint8_t inbound[kSessionCount][kHAPIPSession_DefaultInboundBufferSize];
static uint8_t outbound[kSessionCount][kHAPIPSession_DefaultOutboundBufferSize];
static HAPIPEventNotificationRef notifications[kSessionCount][kAttributeCapacity];
static HAPIPReadContextRef readContexts[kAttributeCapacity];
static HAPIPWriteContextRef writeContexts[kAttributeCapacity];
static uint8_t scratch[kHAPIPSession_DefaultScratchBufferSize];
static HAPAccessory bridgeAccessory;
static HAPAccessory accessories[kMaxDevices];
static HAPAccessory lockSensorAccessory;
static HAPAccessory elevatorAccessories[2];
static const HAPAccessory* accessoryPointers[kMaxDevices + kMaxExtraAccessories + 1];
static const HAPService* bridgeServices[] = {
    &accessoryInformationService, &hapProtocolInformationService, &pairingService, NULL
};
static const HAPService* accessoryServices[kMaxDevices][3];
static HAPBoolCharacteristic switchOnCharacteristics[kMaxDevices];
static const HAPCharacteristic* switchCharacteristics[kMaxDevices][3];
static HAPService switchServices[kMaxDevices];
static HAPBoolCharacteristic lightOnCharacteristics[kMaxDevices];
static HAPIntCharacteristic brightnessCharacteristics[kMaxDevices];
static HAPUInt32Characteristic colorTemperatureCharacteristics[kMaxDevices];
static HAPUInt8Characteristic coverCurrentPositionCharacteristics[kMaxDevices];
static HAPUInt8Characteristic coverTargetPositionCharacteristics[kMaxDevices];
static HAPUInt8Characteristic coverPositionStateCharacteristics[kMaxDevices];
static HAPDataCharacteristic sensorServiceSignatures[kMaxDevices];
static HAPStringCharacteristic sensorNameCharacteristics[kMaxDevices];
static HAPUInt8Characteristic sensorDetectedCharacteristics[kMaxDevices];
static HAPDataCharacteristic thermostatServiceSignatures[kMaxDevices];
static HAPStringCharacteristic thermostatNameCharacteristics[kMaxDevices];
static HAPUInt8Characteristic thermostatCurrentStateCharacteristics[kMaxDevices];
static HAPUInt8Characteristic thermostatTargetStateCharacteristics[kMaxDevices];
static HAPFloatCharacteristic currentTemperatureCharacteristics[kMaxDevices];
static HAPFloatCharacteristic targetTemperatureCharacteristics[kMaxDevices];
static HAPUInt8Characteristic temperatureDisplayUnitsCharacteristics[kMaxDevices];
static const HAPCharacteristic* featureCharacteristics[kMaxDevices][8];
static HAPService featureServices[kMaxDevices];
enum {
    kEventOn, kEventBrightness, kEventColorTemperature, kEventCoverCurrent, kEventCoverTarget,
    kEventCoverState, kEventThermostatCurrentState, kEventThermostatTargetState,
    kEventCurrentTemperature, kEventTargetTemperature, kEventSensor, kNumEventCharacteristics
};
static const HAPCharacteristic* eventCharacteristics[kMaxDevices][kNumEventCharacteristics];
static const HAPService* eventServices[kMaxDevices];
static bool hapReady;
static HAPPlatformTimerRef gatewayTimer;
static HAPPlatformTimerRef lockSensorTimer;
static HAPUInt8Characteristic lockOccupancyCharacteristic;
static HAPDataCharacteristic lockSensorServiceSignature;
static const HAPCharacteristic* lockSensorCharacteristics[3];
static HAPService lockSensorService;
static const HAPService* lockSensorAccessoryServices[3];
static HAPBoolCharacteristic elevatorOnCharacteristics[2];
static const HAPCharacteristic* elevatorSwitchCharacteristics[2][3];
static HAPService elevatorSwitchServices[2];
static const HAPService* elevatorAccessoryServices[2][3];
static uint8_t lastLockSensorValue;

#define kLockSensorAID UINT64_C(0x4C4F434B)

#define kAccessorySchemaVersion "9\n"
#define kAccessorySchemaMarker "config/homekit-schema-version"

static bool AccessorySchemaNeedsUpdate(void) {
    char version[sizeof kAccessorySchemaVersion];
    int fd = open(kAccessorySchemaMarker, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              (st.st_mode & 0077) == 0 && st.st_size == (off_t) sizeof kAccessorySchemaVersion - 1;
    size_t count = 0;
    while (ok && count < sizeof version - 1) {
        ssize_t n = read(fd, version + count, sizeof version - 1 - count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        count += (size_t) n;
    }
    close(fd);
    if (!ok) return true;
    version[count] = '\0';
    return strcmp(version, kAccessorySchemaVersion) != 0;
}

static bool StoreAccessorySchemaVersion(void) {
    int fd = open(kAccessorySchemaMarker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    const size_t length = sizeof kAccessorySchemaVersion - 1;
    size_t count = 0;
    while (count < length) {
        ssize_t n = write(fd, &kAccessorySchemaVersion[count], length - count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        count += (size_t) n;
    }
    bool ok = fchmod(fd, 0600) == 0 && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

static HAPCharacteristicProperties NumericEventProperties(bool writable) {
    return (HAPCharacteristicProperties) {
        .readable = true, .writable = writable, .supportsEventNotification = true,
        .hidden = false, .requiresTimedWrite = false, .supportsAuthorizationData = false,
        .ip = { .controlPoint = false, .supportsWriteResponse = false },
        .ble = { .supportsBroadcastNotification = false, .supportsDisconnectedNotification = false,
                 .readableWithoutSecurity = false, .writableWithoutSecurity = false }
    };
}

static HAPUInt8Characteristic MakeUInt8Characteristic(
        uint64_t iid, const HAPUUID* type, const char* description, bool writable,
        HAPError (*readCallback)(HAPAccessoryServerRef*, const HAPUInt8CharacteristicReadRequest*, uint8_t*, void*),
        HAPError (*writeCallback)(HAPAccessoryServerRef*, const HAPUInt8CharacteristicWriteRequest*, uint8_t, void*)) {
    return (HAPUInt8Characteristic) {
        .format = kHAPCharacteristicFormat_UInt8, .iid = iid, .characteristicType = type,
        .debugDescription = description, .manufacturerDescription = NULL,
        .properties = NumericEventProperties(writable), .units = kHAPCharacteristicUnits_Percentage,
        .constraints = { .minimumValue = 0, .maximumValue = 100, .stepValue = 1,
                         .validValues = NULL, .validValuesRanges = NULL },
        .callbacks = { .handleRead = readCallback, .handleWrite = writeCallback }
    };
}

static void HandleUpdatedState(HAPAccessoryServerRef* server, void* context) {
    (void)context;
    if (HAPAccessoryServerGetState(server) == kHAPAccessoryServerState_Running)
        HAPLogInfo(&kHAPLog_Default, "IHG2 panel HAP bridge is running.");
}

static void SaveSetupInfo(void) {
    HAPError err;
    HAPSetupInfo info;
    bool found;
    size_t numBytes;
    err = HAPPlatformKeyValueStoreGet(&keyValueStore, kSDKKeyValueStoreDomain_Provisioning,
                                      kSDKKeyValueStoreKey_Provisioning_SetupInfo, &info, sizeof info,
                                      &numBytes, &found);
    if (err || (found && numBytes != sizeof info)) HAPFatalError();
    if (!found) {
        HAPSetupCode code;
        HAPSetupID setupID;
        HAPAccessorySetupGenerateRandomSetupCode(&code);
        HAPAccessorySetupGenerateRandomSetupID(&setupID);
        HAPPlatformRandomNumberFill(info.salt, sizeof info.salt);
        static const uint8_t user[] = "Pair-Setup";
        HAP_srp_verifier(info.verifier, info.salt, user, sizeof user - 1,
                         (const uint8_t*)code.stringValue, sizeof code.stringValue - 1);
        err = HAPPlatformKeyValueStoreSet(&keyValueStore, kSDKKeyValueStoreDomain_Provisioning,
                                          kSDKKeyValueStoreKey_Provisioning_SetupInfo, &info, sizeof info);
        if (err) HAPFatalError();
        err = HAPPlatformKeyValueStoreSet(&keyValueStore, kSDKKeyValueStoreDomain_Provisioning,
                                          kSDKKeyValueStoreKey_Provisioning_SetupID, &setupID, sizeof setupID);
        if (err) HAPFatalError();
        err = HAPPlatformKeyValueStoreSet(&keyValueStore, kSDKKeyValueStoreDomain_Provisioning,
                                          kSDKKeyValueStoreKey_Provisioning_SetupCode, &code, sizeof code);
        if (err) HAPFatalError();
        FILE* pin = fopen("config/pairing-code.txt", "w");
        if (!pin) HAPFatalError();
        (void)fchmod(fileno(pin), 0600);
        if (fprintf(pin, "%s\n", code.stringValue) < 0 || fclose(pin) != 0) HAPFatalError();
    }
}

static void GatewayTimer(HAPPlatformTimerRef timer, void* context) {
    (void)timer; (void)context;
    ihg2_arm_poll();
    HAPTime deadline = HAPPlatformClockGetCurrent() + kGatewayPollInterval;
    if (HAPPlatformTimerRegister(&gatewayTimer, deadline, GatewayTimer, NULL)) HAPFatalError();
}

static void LockSensorTimer(HAPPlatformTimerRef timer, void* context) {
    (void)timer; (void)context;
    const uint8_t value = ihg2_arm_lock_sensor_online() ? 1 : 0;
    if (hapReady && value != lastLockSensorValue) {
        lastLockSensorValue = value;
        HAPAccessoryServerRaiseEvent(&accessoryServer,
                                     (const HAPCharacteristic*) &lockOccupancyCharacteristic,
                                     &lockSensorService, &lockSensorAccessory);
    }
    if (HAPPlatformTimerRegister(&lockSensorTimer, HAPPlatformClockGetCurrent() + kLockSensorPollInterval,
                                 LockSensorTimer, NULL)) HAPFatalError();
}

void ihg2_hap_state_changed(size_t index, uint32_t changedMask) {
    if (!hapReady || index >= ihg2_arm_device_count()) return;
    static const uint32_t eventMasks[kNumEventCharacteristics] = {
        IHG2_HAP_STATE_ON, IHG2_HAP_STATE_BRIGHTNESS, IHG2_HAP_STATE_COLOR_TEMPERATURE,
        IHG2_HAP_STATE_COVER_CURRENT, IHG2_HAP_STATE_COVER_TARGET, IHG2_HAP_STATE_COVER_POSITION_STATE,
        IHG2_HAP_STATE_THERMOSTAT_CURRENT_STATE, IHG2_HAP_STATE_THERMOSTAT_TARGET_STATE,
        IHG2_HAP_STATE_CURRENT_TEMPERATURE, IHG2_HAP_STATE_TARGET_TEMPERATURE, IHG2_HAP_STATE_SENSOR
    };
    for (size_t event = 0; event < kNumEventCharacteristics; event++) {
        if ((changedMask & eventMasks[event]) && eventCharacteristics[index][event]) {
            HAPAccessoryServerRaiseEvent(&accessoryServer, eventCharacteristics[index][event],
                                         eventServices[index], &accessories[index]);
        }
    }
}

void ihg2_hap_elevator_state_changed(size_t direction) {
    if (!hapReady || direction >= 2 || ihg2_arm_elevator_available() == 0) return;
    HAPAccessoryServerRaiseEvent(&accessoryServer,
                                 (const HAPCharacteristic*) &elevatorOnCharacteristics[direction],
                                 &elevatorSwitchServices[direction],
                                 &elevatorAccessories[direction]);
}

int main(int argc, char* argv[]) {
    if (argc == 5 && strcmp(argv[1], "--log-writer") == 0) {
        return ihg2_arm_log_writer(argv[2], argv[3], argv[4]);
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts(kBridgeVersionText);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--build-mode") == 0) {
        puts("IHG2_ARM_AUTO_ONLY_V1");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--check-panel-sdk") == 0) {
        size_t count = 0;
        if (ihg2_arm_panel_adoption_probe(&count) != 1) {
            fprintf(stderr, "Panel SDK auto-discovery is not ready; open the vendor app and confirm its gateway is online.\n");
            return 1;
        }
        printf("Panel SDK auto-discovery ready; supported accessories: %zu\n", count);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--doctor") == 0) {
        puts(kBridgeVersionText);
        return ihg2_arm_doctor("/data/local/ihg2-arm-native");
    }
    if (argc == 4 && strcmp(argv[1], "--configure-lock") == 0)
        return ihg2_arm_configure_lock("/data/local/ihg2-arm-native", argv[2], argv[3]);
    if (argc != 1) {
        fprintf(stderr, "Usage: ihg2-arm-bridge [--version|--doctor|--check-panel-sdk|--build-mode|--configure-lock IP MAC]\n");
        return 2;
    }
    if (ihg2_arm_initialize("." ) != 1) {
        fprintf(stderr, "ihg2-arm: panel SDK auto-discovery or gateway socket initialization failed\n");
        return 2;
    }
    const size_t count = ihg2_arm_device_count();
    if (!count || count > kMaxDevices) return 2;
    const bool elevatorAvailable = ihg2_arm_elevator_available() != 0;
    (void)mkdir("config", 0700);
    (void)mkdir(".hap_store", 0700);
    HAPPlatformKeyValueStoreCreate(&keyValueStore, &(const HAPPlatformKeyValueStoreOptions){ .rootDirectory = ".hap_store" });
    HAPPlatformAccessorySetupCreate(&accessorySetup, &(const HAPPlatformAccessorySetupOptions){ .keyValueStore = &keyValueStore });
    HAPPlatformRunLoopCreate(&(const HAPPlatformRunLoopOptions){ .keyValueStore = &keyValueStore });
    HAPPlatformTCPStreamManagerCreate(&tcpStreamManager, &(const HAPPlatformTCPStreamManagerOptions){
        .interfaceName = NULL, .port = ihg2_arm_homekit_port(), .maxConcurrentTCPStreams = kSessionCount
    });
    HAPPlatformServiceDiscoveryCreate(&serviceDiscovery, &(const HAPPlatformServiceDiscoveryOptions){ .interfaceName = NULL });
    hapPlatform.keyValueStore = &keyValueStore;
    hapPlatform.accessorySetup = &accessorySetup;
    hapPlatform.ip.tcpStreamManager = &tcpStreamManager;
    hapPlatform.ip.serviceDiscovery = &serviceDiscovery;
    SaveSetupInfo();

    for (size_t i = 0; i < kSessionCount; i++) {
        sessions[i].inboundBuffer.bytes = inbound[i];
        sessions[i].inboundBuffer.numBytes = sizeof inbound[i];
        sessions[i].outboundBuffer.bytes = outbound[i];
        sessions[i].outboundBuffer.numBytes = sizeof outbound[i];
        sessions[i].eventNotifications = notifications[i];
        sessions[i].numEventNotifications = kAttributeCapacity;
    }
    static HAPIPAccessoryServerStorage ipStorage;
    ipStorage.sessions = sessions;
    ipStorage.numSessions = kSessionCount;
    ipStorage.readContexts = readContexts;
    ipStorage.numReadContexts = kAttributeCapacity;
    ipStorage.writeContexts = writeContexts;
    ipStorage.numWriteContexts = kAttributeCapacity;
    ipStorage.scratchBuffer.bytes = scratch;
    ipStorage.scratchBuffer.numBytes = sizeof scratch;
    serverOptions.maxPairings = kHAPPairingStorage_MinElements;
    serverOptions.ip.transport = &kHAPAccessoryServerTransport_IP;
    serverOptions.ip.accessoryServerStorage = &ipStorage;
    serverCallbacks.handleUpdatedState = HandleUpdatedState;

    bridgeAccessory = (HAPAccessory){
        .aid = 1, .category = kHAPAccessoryCategory_Bridges,
        .name = ihg2_arm_homekit_name(), .manufacturer = "IHG2",
        .model = "Panel Native", .serialNumber = "IHG2-PANEL-01", .firmwareVersion = IHG2_ARM_VERSION,
        .hardwareVersion = "1.0.0", .services = bridgeServices,
        .callbacks = { .identify = IdentifyAccessory }
    };
    for (size_t i = 0; i < count; i++) {
        const bool light = ihg2_arm_device_is_light(i) != 0;
        const bool cover = ihg2_arm_device_is_cover(i) != 0 && ihg2_arm_device_has_cover_position(i) != 0;
        const bool climate = ihg2_arm_device_is_climate(i) != 0;
        const bool binarySensor = ihg2_arm_device_is_binary_sensor(i) != 0;
        const bool leakSensor = binarySensor && ihg2_arm_device_sensor_is_leak(i) != 0;
        const bool canWrite = ihg2_arm_device_can_write(i) != 0;
        const HAPService* mainService = NULL;
        if (light) {
            lightOnCharacteristics[i] = lightBulbOnCharacteristic;
            if (!canWrite) {
                lightOnCharacteristics[i].properties.writable = false;
                lightOnCharacteristics[i].callbacks.handleWrite = NULL;
            }
            size_t characteristicIndex = 0;
            featureCharacteristics[i][characteristicIndex++] = lightBulbService.characteristics[0];
            featureCharacteristics[i][characteristicIndex++] = lightBulbService.characteristics[1];
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &lightOnCharacteristics[i];
            eventCharacteristics[i][kEventOn] = (const HAPCharacteristic*) &lightOnCharacteristics[i];
            if (ihg2_arm_device_has_brightness(i)) {
                brightnessCharacteristics[i] = (HAPIntCharacteristic) {
                    .format = kHAPCharacteristicFormat_Int, .iid = 0x0034,
                    .characteristicType = &kHAPCharacteristicType_Brightness,
                    .debugDescription = kHAPCharacteristicDebugDescription_Brightness,
                    .manufacturerDescription = NULL,
                    .properties = NumericEventProperties(canWrite),
                    .units = kHAPCharacteristicUnits_Percentage,
                    .constraints = { .minimumValue = 0, .maximumValue = 100, .stepValue = 1 },
                    .callbacks = { .handleRead = HandleBrightnessRead,
                                   .handleWrite = canWrite ? HandleBrightnessWrite : NULL }
                };
                featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &brightnessCharacteristics[i];
                eventCharacteristics[i][kEventBrightness] = (const HAPCharacteristic*) &brightnessCharacteristics[i];
            }
            if (ihg2_arm_device_has_color_temperature(i)) {
                colorTemperatureCharacteristics[i] = (HAPUInt32Characteristic) {
                    .format = kHAPCharacteristicFormat_UInt32, .iid = 0x0035,
                    .characteristicType = &kHAPCharacteristicType_ColorTemperature,
                    .debugDescription = kHAPCharacteristicDebugDescription_ColorTemperature,
                    .manufacturerDescription = NULL,
                    .properties = NumericEventProperties(canWrite),
                    .units = kHAPCharacteristicUnits_None,
                    .constraints = { .minimumValue = 50, .maximumValue = 400, .stepValue = 1 },
                    .callbacks = { .handleRead = HandleColorTemperatureRead,
                                   .handleWrite = canWrite ? HandleColorTemperatureWrite : NULL }
                };
                featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &colorTemperatureCharacteristics[i];
                eventCharacteristics[i][kEventColorTemperature] = (const HAPCharacteristic*) &colorTemperatureCharacteristics[i];
            }
            featureCharacteristics[i][characteristicIndex] = NULL;
            featureServices[i] = lightBulbService;
            featureServices[i].characteristics = featureCharacteristics[i];
            mainService = &featureServices[i];
            eventServices[i] = mainService;
        } else if (cover) {
            size_t characteristicIndex = 0;
            featureCharacteristics[i][characteristicIndex++] = lightBulbService.characteristics[0];
            featureCharacteristics[i][characteristicIndex++] = lightBulbService.characteristics[1];
            coverCurrentPositionCharacteristics[i] = MakeUInt8Characteristic(
                    0x0034, &kHAPCharacteristicType_CurrentPosition, kHAPCharacteristicDebugDescription_CurrentPosition,
                    false, HandleCoverCurrentPositionRead, NULL);
            coverTargetPositionCharacteristics[i] = MakeUInt8Characteristic(
                    0x0035, &kHAPCharacteristicType_TargetPosition, kHAPCharacteristicDebugDescription_TargetPosition,
                    canWrite, HandleCoverTargetPositionRead, canWrite ? HandleCoverTargetPositionWrite : NULL);
            coverPositionStateCharacteristics[i] = MakeUInt8Characteristic(
                    0x0036, &kHAPCharacteristicType_PositionState, kHAPCharacteristicDebugDescription_PositionState,
                    false, HandleCoverPositionStateRead, NULL);
            coverPositionStateCharacteristics[i].units = kHAPCharacteristicUnits_None;
            coverPositionStateCharacteristics[i].constraints.maximumValue = 2;
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &coverCurrentPositionCharacteristics[i];
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &coverTargetPositionCharacteristics[i];
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &coverPositionStateCharacteristics[i];
            featureCharacteristics[i][characteristicIndex] = NULL;
            featureServices[i] = (HAPService) {
                .iid = 0x0030, .serviceType = &kHAPServiceType_WindowCovering,
                .debugDescription = kHAPServiceDebugDescription_WindowCovering,
                .name = ihg2_arm_device_name(i),
                .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
                .linkedServices = NULL, .characteristics = featureCharacteristics[i]
            };
            eventCharacteristics[i][kEventCoverCurrent] = (const HAPCharacteristic*) &coverCurrentPositionCharacteristics[i];
            eventCharacteristics[i][kEventCoverTarget] = (const HAPCharacteristic*) &coverTargetPositionCharacteristics[i];
            eventCharacteristics[i][kEventCoverState] = (const HAPCharacteristic*) &coverPositionStateCharacteristics[i];
            mainService = &featureServices[i];
            eventServices[i] = mainService;
        } else if (climate) {
            size_t characteristicIndex = 0;
            thermostatServiceSignatures[i] = *(const HAPDataCharacteristic*) lightBulbService.characteristics[0];
            thermostatServiceSignatures[i].iid = 0x0041;
            thermostatNameCharacteristics[i] = *(const HAPStringCharacteristic*) lightBulbService.characteristics[1];
            thermostatNameCharacteristics[i].iid = 0x0042;
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &thermostatServiceSignatures[i];
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &thermostatNameCharacteristics[i];
            thermostatCurrentStateCharacteristics[i] = MakeUInt8Characteristic(
                    0x0043, &kHAPCharacteristicType_CurrentHeatingCoolingState,
                    kHAPCharacteristicDebugDescription_CurrentHeatingCoolingState, false,
                    HandleThermostatCurrentStateRead, NULL);
            thermostatCurrentStateCharacteristics[i].units = kHAPCharacteristicUnits_None;
            thermostatCurrentStateCharacteristics[i].constraints.maximumValue = 2;
            thermostatTargetStateCharacteristics[i] = MakeUInt8Characteristic(
                    0x0044, &kHAPCharacteristicType_TargetHeatingCoolingState,
                    kHAPCharacteristicDebugDescription_TargetHeatingCoolingState, canWrite,
                    HandleThermostatTargetStateRead, canWrite ? HandleThermostatTargetStateWrite : NULL);
            thermostatTargetStateCharacteristics[i].units = kHAPCharacteristicUnits_None;
            thermostatTargetStateCharacteristics[i].constraints.maximumValue = 3;
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &thermostatCurrentStateCharacteristics[i];
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &thermostatTargetStateCharacteristics[i];
            currentTemperatureCharacteristics[i] = (HAPFloatCharacteristic) {
                .format = kHAPCharacteristicFormat_Float, .iid = 0x0045,
                .characteristicType = &kHAPCharacteristicType_CurrentTemperature,
                .debugDescription = kHAPCharacteristicDebugDescription_CurrentTemperature,
                .manufacturerDescription = NULL,
                .properties = NumericEventProperties(false), .units = kHAPCharacteristicUnits_Celsius,
                .constraints = { .minimumValue = 0.0F, .maximumValue = 100.0F, .stepValue = 0.1F },
                .callbacks = { .handleRead = HandleCurrentTemperatureRead, .handleWrite = NULL }
            };
            targetTemperatureCharacteristics[i] = (HAPFloatCharacteristic) {
                .format = kHAPCharacteristicFormat_Float, .iid = 0x0046,
                .characteristicType = &kHAPCharacteristicType_TargetTemperature,
                .debugDescription = kHAPCharacteristicDebugDescription_TargetTemperature,
                .manufacturerDescription = NULL,
                .properties = NumericEventProperties(canWrite), .units = kHAPCharacteristicUnits_Celsius,
                .constraints = { .minimumValue = 10.0F, .maximumValue = 38.0F, .stepValue = 1.0F },
                .callbacks = { .handleRead = HandleTargetTemperatureRead,
                               .handleWrite = canWrite ? HandleTargetTemperatureWrite : NULL }
            };
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &currentTemperatureCharacteristics[i];
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &targetTemperatureCharacteristics[i];
            temperatureDisplayUnitsCharacteristics[i] = MakeUInt8Characteristic(
                    0x0047, &kHAPCharacteristicType_TemperatureDisplayUnits,
                    kHAPCharacteristicDebugDescription_TemperatureDisplayUnits, canWrite,
                    HandleTemperatureDisplayUnitsRead, canWrite ? HandleTemperatureDisplayUnitsWrite : NULL);
            temperatureDisplayUnitsCharacteristics[i].units = kHAPCharacteristicUnits_None;
            temperatureDisplayUnitsCharacteristics[i].constraints.maximumValue = 1;
            featureCharacteristics[i][characteristicIndex++] = (const HAPCharacteristic*) &temperatureDisplayUnitsCharacteristics[i];
            featureCharacteristics[i][characteristicIndex] = NULL;
            featureServices[i] = (HAPService) {
                .iid = 0x0040, .serviceType = &kHAPServiceType_Thermostat,
                .debugDescription = kHAPServiceDebugDescription_Thermostat, .name = "Thermostat",
                .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
                .linkedServices = NULL, .characteristics = featureCharacteristics[i]
            };
            eventCharacteristics[i][kEventThermostatCurrentState] = (const HAPCharacteristic*) &thermostatCurrentStateCharacteristics[i];
            eventCharacteristics[i][kEventThermostatTargetState] = (const HAPCharacteristic*) &thermostatTargetStateCharacteristics[i];
            eventCharacteristics[i][kEventCurrentTemperature] = (const HAPCharacteristic*) &currentTemperatureCharacteristics[i];
            eventCharacteristics[i][kEventTargetTemperature] = (const HAPCharacteristic*) &targetTemperatureCharacteristics[i];
            mainService = &featureServices[i];
            eventServices[i] = mainService;
        } else if (binarySensor) {
            // Use fresh IIDs when changing a previously exposed Switch service to a sensor.
            // Controllers may cache the old On characteristic by IID even after the service type changes.
            sensorServiceSignatures[i] = *(const HAPDataCharacteristic*) lightBulbService.characteristics[0];
            sensorServiceSignatures[i].iid = 0x0051;
            sensorNameCharacteristics[i] = *(const HAPStringCharacteristic*) lightBulbService.characteristics[1];
            sensorNameCharacteristics[i].iid = 0x0052;
            featureCharacteristics[i][0] = (const HAPCharacteristic*) &sensorServiceSignatures[i];
            featureCharacteristics[i][1] = (const HAPCharacteristic*) &sensorNameCharacteristics[i];
            sensorDetectedCharacteristics[i] = MakeUInt8Characteristic(
                    0x0053,
                    leakSensor ? &kHAPCharacteristicType_LeakDetected : &kHAPCharacteristicType_OccupancyDetected,
                    leakSensor ? kHAPCharacteristicDebugDescription_LeakDetected
                               : kHAPCharacteristicDebugDescription_OccupancyDetected,
                    false, HandleDeviceSensorRead, NULL);
            sensorDetectedCharacteristics[i].units = kHAPCharacteristicUnits_None;
            sensorDetectedCharacteristics[i].constraints.maximumValue = 1;
            featureCharacteristics[i][2] = (const HAPCharacteristic*) &sensorDetectedCharacteristics[i];
            featureCharacteristics[i][3] = NULL;
            featureServices[i] = (HAPService) {
                .iid = 0x0050,
                .serviceType = leakSensor ? &kHAPServiceType_LeakSensor : &kHAPServiceType_OccupancySensor,
                .debugDescription = leakSensor ? kHAPServiceDebugDescription_LeakSensor
                                               : kHAPServiceDebugDescription_OccupancySensor,
                .name = leakSensor ? "Leak Sensor" : "Occupancy Sensor",
                .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
                .linkedServices = NULL, .characteristics = featureCharacteristics[i]
            };
            eventCharacteristics[i][kEventSensor] = (const HAPCharacteristic*) &sensorDetectedCharacteristics[i];
            mainService = &featureServices[i];
            eventServices[i] = mainService;
        } else {
            switchServices[i] = ihg2SwitchService;
            switchOnCharacteristics[i] = *(const HAPBoolCharacteristic*)ihg2SwitchService.characteristics[1];
            if (!canWrite) {
                switchOnCharacteristics[i].properties.writable = false;
                switchOnCharacteristics[i].callbacks.handleWrite = NULL;
            }
            switchCharacteristics[i][0] = ihg2SwitchService.characteristics[0];
            switchCharacteristics[i][1] = (const HAPCharacteristic*)&switchOnCharacteristics[i];
            switchCharacteristics[i][2] = NULL;
            switchServices[i].characteristics = switchCharacteristics[i];
            mainService = &switchServices[i];
            eventCharacteristics[i][kEventOn] = (const HAPCharacteristic*) &switchOnCharacteristics[i];
            eventServices[i] = mainService;
        }
        accessoryServices[i][0] = &accessoryInformationService;
        accessoryServices[i][1] = mainService;
        accessoryServices[i][2] = NULL;
        accessories[i] = (HAPAccessory){
            .aid = ihg2_arm_device_aid(i), .category = kHAPAccessoryCategory_BridgedAccessory,
            .name = ihg2_arm_device_name(i), .manufacturer = "IHG2",
            .model = cover ? "Gateway Window Covering" :
                     (climate ? "Gateway Thermostat" :
                      (light ? "Gateway Light" :
                       (binarySensor ? (leakSensor ? "Gateway Leak Sensor" : "Gateway Occupancy Sensor")
                                     : "Gateway Switch"))),
            .serialNumber = ihg2_arm_device_name(i), .firmwareVersion = IHG2_ARM_VERSION,
            .hardwareVersion = "1.0.0", .services = accessoryServices[i],
            .callbacks = { .identify = IdentifyAccessory }
        };
        accessoryPointers[i] = &accessories[i];
        if (ihg2_arm_device_aid(i) == kLockSensorAID ||
            ihg2_arm_device_aid(i) == IHG2_ARM_ELEVATOR_UP_AID ||
            ihg2_arm_device_aid(i) == IHG2_ARM_ELEVATOR_DOWN_AID) {
            fprintf(stderr, "ihg2-arm: HomeKit accessory identifier collision\n");
            return 2;
        }
    }
    size_t accessoryIndex = count;
    if (elevatorAvailable) {
        static const char* const names[] = { "呼梯上行", "呼梯下行" };
        static const char* const serialNumbers[] = { "IHG2-ELEVATOR-UP", "IHG2-ELEVATOR-DOWN" };
        static const uint64_t aids[] = {
            IHG2_ARM_ELEVATOR_UP_AID, IHG2_ARM_ELEVATOR_DOWN_AID
        };
        for (size_t direction = 0; direction < 2; direction++) {
            elevatorSwitchServices[direction] = ihg2SwitchService;
            elevatorOnCharacteristics[direction] =
                    *(const HAPBoolCharacteristic*) ihg2SwitchService.characteristics[1];
            elevatorSwitchCharacteristics[direction][0] = ihg2SwitchService.characteristics[0];
            elevatorSwitchCharacteristics[direction][1] =
                    (const HAPCharacteristic*) &elevatorOnCharacteristics[direction];
            elevatorSwitchCharacteristics[direction][2] = NULL;
            elevatorSwitchServices[direction].characteristics = elevatorSwitchCharacteristics[direction];
            elevatorAccessoryServices[direction][0] = &accessoryInformationService;
            elevatorAccessoryServices[direction][1] = &elevatorSwitchServices[direction];
            elevatorAccessoryServices[direction][2] = NULL;
            elevatorAccessories[direction] = (HAPAccessory) {
                .aid = aids[direction], .category = kHAPAccessoryCategory_BridgedAccessory,
                .name = names[direction], .manufacturer = "Leelen",
                .model = "Native Elevator Call", .serialNumber = serialNumbers[direction],
                .firmwareVersion = IHG2_ARM_VERSION, .hardwareVersion = "1.0.0",
                .services = elevatorAccessoryServices[direction],
                .callbacks = { .identify = IdentifyAccessory }
            };
            accessoryPointers[accessoryIndex++] = &elevatorAccessories[direction];
        }
    }
    lockSensorServiceSignature = (HAPDataCharacteristic) {
        .format = kHAPCharacteristicFormat_Data, .iid = 0x0031,
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
    lockOccupancyCharacteristic = (HAPUInt8Characteristic) {
        .format = kHAPCharacteristicFormat_UInt8, .iid = 0x0033,
        .characteristicType = &kHAPCharacteristicType_OccupancyDetected,
        .debugDescription = kHAPCharacteristicDebugDescription_OccupancyDetected,
        .manufacturerDescription = NULL,
        .properties = NumericEventProperties(false), .units = kHAPCharacteristicUnits_None,
        .constraints = { .minimumValue = 0, .maximumValue = 1, .stepValue = 1,
                         .validValues = NULL, .validValuesRanges = NULL },
        .callbacks = { .handleRead = HandleLockOccupancyRead, .handleWrite = NULL }
    };
    lockSensorCharacteristics[0] = (const HAPCharacteristic*) &lockSensorServiceSignature;
    lockSensorCharacteristics[1] = (const HAPCharacteristic*) &lockOccupancyCharacteristic;
    lockSensorCharacteristics[2] = NULL;
    lockSensorService = (HAPService) {
        .iid = 0x0030, .serviceType = &kHAPServiceType_OccupancySensor,
        .debugDescription = kHAPServiceDebugDescription_OccupancySensor,
        .name = "Occupancy Sensor",
        .properties = { .primaryService = true, .hidden = false, .ble = { .supportsConfiguration = false } },
        .linkedServices = NULL, .characteristics = lockSensorCharacteristics
    };
    lockSensorAccessoryServices[0] = &accessoryInformationService;
    lockSensorAccessoryServices[1] = &lockSensorService;
    lockSensorAccessoryServices[2] = NULL;
    lockSensorAccessory = (HAPAccessory) {
        .aid = kLockSensorAID, .category = kHAPAccessoryCategory_BridgedAccessory,
        .name = "门锁网络活动", .manufacturer = "IHG2",
        .model = "Wi-Fi Presence Sensor", .serialNumber = "IHG2-LOCK-WIFI-01",
        .firmwareVersion = IHG2_ARM_VERSION, .hardwareVersion = "1.0.0",
        .services = lockSensorAccessoryServices,
        .callbacks = { .identify = IdentifyAccessory }
    };
    accessoryPointers[accessoryIndex++] = &lockSensorAccessory;
    accessoryPointers[accessoryIndex] = NULL;
    HAPAccessoryServerCreate(&accessoryServer, &serverOptions, &hapPlatform, &serverCallbacks, NULL);
    const bool configurationChanged = AccessorySchemaNeedsUpdate();
    HAPAccessoryServerStartBridge(&accessoryServer, &bridgeAccessory, accessoryPointers, configurationChanged);
    if (configurationChanged && !StoreAccessorySchemaVersion()) {
        HAPLogError(&kHAPLog_Default, "Could not persist the HomeKit accessory schema version.");
        return 2;
    }
    hapReady = true;
    lastLockSensorValue = ihg2_arm_lock_sensor_online() ? 1 : 0;
    if (HAPPlatformTimerRegister(&gatewayTimer, HAPPlatformClockGetCurrent() + kGatewayPollInterval, GatewayTimer, NULL)) HAPFatalError();
    if (HAPPlatformTimerRegister(&lockSensorTimer, HAPPlatformClockGetCurrent() + kLockSensorPollInterval,
                                 LockSensorTimer, NULL)) HAPFatalError();
    HAPPlatformRunLoopRun();
    HAPAccessoryServerRelease(&accessoryServer);
    HAPPlatformRunLoopRelease();
    return 0;
}
