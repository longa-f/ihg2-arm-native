# Android target deployment

The release bundle targets a rooted RK3288-class panel with Android API 25 or newer, 32-bit `armeabi-v7a`, `/system/lib/libmdnssd.so`, and an init service that launches `/system/bin/install-recovery.sh` as `flash_recovery`. It is not a generic installer for arbitrary Android devices. The installer checks these prerequisites and refuses to replace an unrelated recovery hook.

Build the ARM service, then package it without household credentials:

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r30 \
OPENSSL_SOURCE=/path/to/openssl-3.5.9 \
MDNS_LIBRARY=/path/to/panel-libmdnssd.so \
make android

ANDROID_NDK_HOME=/path/to/android-ndk-r30 \
OPENSSL_SOURCE=/path/to/openssl-3.5.9 \
MDNS_LIBRARY=/path/to/panel-libmdnssd.so \
make package-android
```

`VERSION` is the release source of truth. Increase it for each software release; the package name includes `-vMAJOR.MINOR.PATCH`, and a matching `.sha256` file is created beside it. The package target refuses to replace an existing version. The bridge prints its version with `--version` and at the start of `--doctor`; the installer checks that the bundle's `VERSION` matches the executable before deployment.

Build and package once for each software release and compatible hardware profile, then copy the same archive to a Windows or Linux maintenance computer at each household. The archive includes the stripped ARM executable, Windows and Linux install/doctor entry points, shared device-side scripts, the supervisor, boot hook, and third-party licenses. It contains no household data or generic manual-config examples. The computer needs Android platform-tools (`adb`); it does not need the Android NDK or a C++ build environment.

The first connection still requires ADB debugging to be enabled on the panel and the computer's ADB key to be authorized. The installer cannot enable ADB remotely. Once the panel is reachable over the LAN, pass its IP to the installer; this panel profile defaults to ADB TCP port `1379`, and an explicit port can be supplied when it differs. The installer runs `adb connect` itself. If ADB is already connected over USB or TCP, omit the target only when exactly one device is online.

On Linux, install a fresh supported panel or update an existing one:

```sh
./install.sh 192.168.5.23
```

The IP above is an example; the script connects to `192.168.5.23:1379`. An explicit TCP port or an already-connected ADB serial is also accepted. To use the only device already listed by `adb devices`, omit the target. On Windows, extract the same archive and run `install.cmd` from Command Prompt or PowerShell:

```powershell
.\install.cmd 192.168.5.23
```

`install.ps1` can be run directly from PowerShell when script execution is permitted.

The native service reads the vendor app's private SDK JSON databases on the panel. It extracts only the gateway AES key, gateway MAC, and accessory inventory needed by the bridge. The key remains on the panel in the vendor app's existing protected storage; it is neither printed nor copied to the install computer. The gateway IP is resolved by matching that MAC in `/proc/net/arp` after the panel SDK has communicated with the gateway. The inventory converter uses the panel's logical channel groups and has been checked against the current 31-accessory installation.

The automatic mode is supported on rooted RK3288 panels with the same vendor app package and SDK file layout, Android API 25 or newer, 32-bit `armeabi-v7a`, `/system/lib/libmdnssd.so`, and the expected init hook. It uses the observed IHG2 protocol constants: gateway UDP port `80`, protocol device ID `1`, and device type `0x274a`. HomeKit defaults to `IHG2 Local Bridge` on TCP port `53760`; pairing identity and setup code are generated and stored on the panel. The vendor app must already be provisioned and have a synchronized device list. If the gateway is not yet in the panel's neighbor table, the installer stops before replacing the running service; open the vendor app and retry after it connects.

Run the deployment diagnostic from the install computer without changing the service:

```sh
./install.sh 192.168.5.23 --doctor
```

On Windows:

```powershell
.\install.cmd 192.168.5.23 -Doctor
```

The deployed binary also supports `--doctor` when run as root on the panel. It checks private SDK data, the supported accessory count, gateway neighbor resolution and heartbeat response, HomeKit TCP and mDNS listeners, the boot hook's SELinux execution label, and pairing-store permissions. It hides gateway credentials and household device identifiers. When all checks pass, it prints the protected HomeKit setup code if the code file is present and valid. Its network test sends one heartbeat and no device-control command.

Before installing, the optional host-side preflight reports panel ABI, Android API, memory, storage, and available boot-hook information:

```sh
./preflight.sh 192.168.5.23
```

On Windows:

```powershell
.\preflight.ps1 192.168.5.23
```

Both entry points can omit the target when exactly one ADB device is already connected. They never copy the household AES key or accessory inventory to the computer. The same install command handles first installation and updates, and preserves Apple Home pairing data.

The installer stages only the generic executable and service scripts under `/data/local/tmp`. It verifies root/ABI/API/mDNS/init support and panel SDK auto-discovery before stopping the running bridge, atomically replaces the executable, and starts the init-managed supervisor. It preserves `/data/local/ihg2-arm-native/.hap_store` and the pairing PIN, removes obsolete `bridge.json`, `gateway.key`, and inventory files after the new automatic bridge is listening, and keeps a copy of the previous executable for service rollback. It then checks `/proc/net/tcp*` for the automatic HomeKit listener on TCP `53760`.

The lock Wi-Fi presence sensor uses a separate per-home runtime config. After installing the bridge, write the lock's current IP and Wi-Fi MAC from the maintenance computer:

```sh
./install.sh PANEL_IP --configure-lock 192.168.1.100 02:00:00:00:00:01
```

Windows uses `install.cmd PANEL_IP -ConfigureLock -LockIp 192.168.1.100 -LockMac 02:00:00:00:00:01`. Replace both values with that home's lock details. The command stores them in `/data/local/ihg2-arm-native/config/lock-sensor.conf`, owned by root with mode `0600`; no household values enter the program or release archive. The running service rereads the file every five seconds, so rerun the command if the lock's IP changes. No rebuild or service restart is needed. The sensor probes only the configured IP with ARP and accepts a reply only when its MAC matches; it never scans the subnet. It reports local network reachability, which can trigger Apple Home automations but does not confirm an unlock. The panel and lock must share a directly connected subnet.

On first pairing, the bridge generates a random HomeKit setup code and stores it root-only at `/data/local/ihg2-arm-native/config/pairing-code.txt`. A healthy `--doctor` run displays the code; it can also be read directly when pairing is needed:

```sh
adb -s DEVICE shell su 0 cat /data/local/ihg2-arm-native/config/pairing-code.txt
```

Each start reads the current key and device list from that panel's vendor SDK and resolves its gateway address locally. The lock sensor's separate private runtime file is optional and manually configured as described above. Existing obsolete gateway config files are ignored by the new binary and removed after a successful update.

The installer briefly remounts `/system` read-write to install or repair the boot hook's SELinux label, then restores read-only. It verifies the `install_recovery_exec` label before starting the Android init service.

The supervisor sends bridge stdout and stderr through the built-in log writer. `bridge.log` and `bridge.log.1` are each capped at 5 MiB, so their combined size stays at or below 10 MiB. Rotation happens while the bridge is running and does not restart the HomeKit service. The bridge creates `.bridge-log.pipe` through the system API, so installation does not depend on a shell `mkfifo` command; the temporary FIFO is removed when the service stops. If storage rejects log writes, the writer drains and discards further output so a full disk cannot block the bridge.

The panel links HomeKit DNS-SD through its firmware's `/system/lib/libmdnssd.so`; OpenSSL and the C++ runtime are statically linked. HAP TCP (default 53760), gateway UDP (default 80), and the bridge's ephemeral UDP source port are separate. The gateway protocol and curtain conversion logic have offline tests; the current device deployment additionally verifies that the HomeKit listener comes up. A physical movement is not triggered by the installer.
