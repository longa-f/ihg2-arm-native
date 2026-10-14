# IHG2 ARM Native Bridge

An isolated, low-resource bridge project for exposing IHG2 gateway accessories to Apple Home from the RK3288 control panel. It reuses the heartbeat and encrypted-frame codec already implemented and tested in the separate C++ bridge; it does not use Home Assistant, MQTT, Homebridge, cloud APIs, or scene editing.

## Target

- Control panel: Android 7.1.2, 32-bit ARMv7 (`armeabi-v7a`), root access.
- Runtime shape: one supervised native process; fixed-size buffers and device tables; no general-purpose broker or scripting runtime.
- Data path: gateway UDP reports/commands ↔ bridge state table ↔ HomeKit HAP over IP.
- Scope: lighting/accessory state and control only. Apple Home automations remain in Apple Home.

## Status

This is a separate, native ARMv7 service for the RK3288 control panel. It uses one event loop, the ported AES-128-CBC gateway codec, a connected UDP socket with an ephemeral source port, and Apple's HAP bridge API. It has no runtime dependency on the current server, Home Assistant, MQTT, Homebridge, Node.js, or cloud APIs. Gateway access and HomeKit are separate transports: UDP to the gateway and TCP 53760 for Apple Home.

The service exposes the inventory through stable HomeKit accessory IDs. Lights use a Light Bulb service with On and, when declared by the gateway, Brightness and Color Temperature. Window coverings use the HomeKit Window Covering service with current position, target position, and movement state; the gateway's reported position is in percent. Air conditioners use the HomeKit Thermostat service with current temperature, target temperature, power, and mode controls. The numeric AC mode map (0 auto, 1 cool, 2 dry, 3 fan, 4 heat) is provisional pending a live mode-change confirmation; HomeKit has no dry or fan target mode. Other controllable entries expose an on/off Switch service, and binary sensors are read-only. Scene editing is outside this bridge's scope.

Color temperature is exposed in HomeKit mirek units and mapped to the gateway's 0–100 `colortemp` field. A user-confirmed warmest setting on the bedroom dimmer strip read back as gateway value `0`, so the direction is inverted: `0` maps to HomeKit's warm endpoint and `100` to its cool endpoint. Intermediate values are currently interpolated linearly; only the warm endpoint has been confirmed live.

The packet codec validates packet bounds, checksums, gateway identity, message type, and decrypted JSON before changing state. The gateway protocol uses AES-CBC and additive checksums, not cryptographic packet authentication. HomeKit pairing itself uses the HAP cryptographic session. The private gateway key and HomeKit pairing store live only on the panel.

The HAP engine is the pinned Apple HomeKit ADK submodule. ARM builds statically link OpenSSL 3.5.9, so they do not use Android's private `libcrypto`; the only panel library dependency is its existing DNS-SD client library, `/system/lib/libmdnssd.so`.

## Build and deploy

### Versioning

`VERSION` is the single source of truth and uses `MAJOR.MINOR.PATCH`. Increase PATCH for bug fixes, MINOR for backward-compatible features, and MAJOR for incompatible changes. The build embeds the number in `--version`, `--doctor`, and HomeKit firmware information. Each release archive includes the same `VERSION`, uses a versioned filename, and has a `.sha256` checksum beside it. The installer checks that the archive version matches the ARM binary, stores it at `/data/local/ihg2-arm-native/VERSION`, and keeps the previous value in `VERSION.rollback` on updates.

For a release, update `VERSION`, run the checks and build, commit the changes, and tag that commit with the same `v`-prefixed number (for example `v1.0.1`). Then distribute the generated archive and its checksum. `make package-android` will not overwrite an existing version; `scripts/package-android.sh --force` is available for an intentional local rebuild.

`make clean test` builds and runs host-side state, protocol-vector, and loopback UDP tests. It requires a C++20 compiler and host OpenSSL development headers/library; nlohmann/json 3.11.3 is vendored under `third_party/`.

`make android` builds the ARMv7 service when `ANDROID_NDK_HOME`, `OPENSSL_SOURCE`, and `MDNS_LIBRARY` are set. Use Android NDK r30, an OpenSSL 3.5.9 source tree, and a copy of the target panel's `/system/lib/libmdnssd.so`. Initialize the pinned HomeKit ADK submodule before building. The build applies the tracked short-nonce compatibility patch in a temporary overlay, leaving the upstream submodule unchanged. The resulting binary targets Android API 25 / `armeabi-v7a`; it statically links OpenSSL and the C++ runtime. Build and package once for each software release and hardware profile, then use that same archive for every compatible household. A household's key, gateway address, and device list are read from its own panel at runtime; they are never compiled into the binary.

For either a first install or an update, the installer adopts the local vendor SDK data automatically. The AES key stays on the panel; the installer does not copy household credentials to the computer or write them into a release archive. The panel must already have the vendor app configured and connected to its IHG2 gateway. The release archive lives on a Windows or Linux maintenance computer and needs Android platform-tools (`adb`). First-time ADB debugging and host authorization must already be enabled on the panel; the installer cannot turn those on remotely.

```sh
scripts/deploy-android.sh 192.168.5.23
```

The IP is an example. The installer automatically connects to ADB TCP port `1379`; supply `192.168.5.23:PORT` if it differs. If exactly one ADB device is already connected over USB or TCP, omit the target. From a release bundle, Linux uses `./install.sh 192.168.5.23`; Windows uses `install.cmd 192.168.5.23`.

This discovers the gateway MAC and AES key from the vendor app's private SDK data, resolves the gateway IP from the panel's neighbor table, and builds the HomeKit inventory from the SDK device list. UDP port `80`, protocol device ID `1`, device type `0x274a`, HomeKit name, and HomeKit port `53760` use the observed RK3288/IHG2 defaults. The bridge keeps its pairing store on the panel. Add it to Apple Home once using the setup code in `config/pairing-code.txt`.

To check an installed panel from the deployment computer, run:

```sh
scripts/deploy-android.sh 192.168.5.23 --doctor
```

From a release bundle, Linux uses `./install.sh 192.168.5.23 --doctor`; Windows uses `install.cmd 192.168.5.23 -Doctor`. Both commands connect over ADB when given the panel IP.

Or run `/data/local/ihg2-arm-native/ihg2-arm-bridge --doctor` as root on the panel. Doctor reports SDK access, inventory count, gateway heartbeat, HomeKit TCP listener, mDNS listener, the boot hook's SELinux execution label, and pairing-store permissions. It hides gateway keys, MAC/IP addresses, and device names. Once all checks pass, it prints the protected HomeKit setup code when available. Its only network probe is one gateway heartbeat; it sends no device-control command.

The same install command updates an existing panel and preserves its Apple Home pairing. The installer removes obsolete local gateway config files after the new automatic service is listening. There is no manual config mode. To build a portable release archive:

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r30 \
OPENSSL_SOURCE=/path/to/openssl-3.5.9 \
MDNS_LIBRARY=/path/to/panel-libmdnssd.so \
make package-android
```

On this build PC, `~/3rd/build-ihg2-arm-native.sh` supplies the local toolchain paths and runs the same versioned package build. A deployed bridge reports its version with `ihg2-arm-bridge --version` or as the first line of `--doctor` output.

Extract the archive on the install computer, optionally run `./preflight.sh 192.168.5.23` on Linux or `preflight.ps1 192.168.5.23` on Windows, then run the matching install command. The archive contains no gateway key, owner inventory, HomeKit pairing data, or current-home configuration. The installer checks the ARM/API/mDNS prerequisites and SDK auto-discovery before stopping a service, preserves existing Apple Home pairing data on updates, removes obsolete local manual config files, refuses to replace an unrelated recovery hook, and keeps a copy of the previous bridge executable for rollback.

This portable build supports rooted RK3288-class panels running Android API 25 or newer with the 32-bit `armeabi-v7a` ABI, `/system/lib/libmdnssd.so`, and the matching `flash_recovery` init hook. Other firmware needs a separately validated boot-hook and DNS-SD adapter. The installer verifies that the automatic HomeKit TCP listener on port `53760` is live before it reports success. The gateway's local UDP source port is ephemeral. After installation, runtime traffic is only between the panel, the gateway, and Apple Home on the LAN; there is no runtime dependency on the development server, Home Assistant, MQTT, Homebridge, or cloud APIs.
