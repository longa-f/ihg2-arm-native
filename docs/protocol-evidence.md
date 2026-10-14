# Gateway protocol implementation and evidence

The Android panel is an observed gateway client: it sends UDP heartbeats to `192.168.5.26:80`, receives heartbeat acknowledgements, and receives unsolicited state reports. The message identifiers are `0x0069`, `0x03ed`, and `0x03ee`. The panel's source UDP port is ephemeral, so the native bridge keeps one connected UDP socket for heartbeat sends and asynchronous reports.

The isolated ARM project now ports `ihg2-cpp`'s `protocol.cpp` and its bounded JSON parsing. It checks the 56-byte outer header, outer checksum, AES-128-CBC envelope, inner magic/length/checksum, configured MAC/device identity, message type, and JSON bounds. Heartbeat generation and encrypted report decoding are exercised with the separate bridge's test-only golden vectors. The response vector contains a `0x03ee` report; the heartbeat-ACK type `0x0000` is covered by a codec round-trip matching the installed-firmware rule.

The port is in `src/vendor_protocol/protocol.cpp` and `include/ihg2_arm/vendor_protocol/protocol.hpp`. Its codec logic is copied from the new C++ bridge; only the include path and namespace were adapted. The test vectors use a synthetic test key (`00..0f`), never the installed gateway key.

These checks establish codec compatibility with the retained fixtures, not full operation on the RK3288 panel. The gateway protocol has no keyed MAC or AEAD tag: its AES-CBC ciphertext is accompanied by additive checksums, so a valid checksum is not cryptographic authentication. The connected UDP socket filters the expected gateway IP/port, and the decoder checks the configured device identity, but those checks do not turn the vendor packet format into an authenticated protocol.

Remaining validation is: command acknowledgements/read-back behavior for each control profile and malformed/unknown-target rejection against target firmware. Keep captures mode `0600`, extract only sanitized fixtures, then remove raw captures; never commit keys or raw packets.

The panel's live query has also returned AC fields for the configured `climate` entries: `tp_acpwr=0`, `ac_mode=0`, `ac_temp` in the range 24–26, `insidetemp` in the range 25–26, and `ac_mark` values 0 or 2 while power was off. This confirms integer Celsius values for the setpoint and room temperature, and an off value of 0. It does not confirm the mode enumeration because the AC was off during capture. The code currently uses the common provisional mapping 0=auto, 1=cool, 2=dry, 3=fan, 4=heat; verify this by observing a mode change before relying on the non-off mode controls.
