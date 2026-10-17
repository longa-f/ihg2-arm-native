# Target architecture and safety properties

## Process boundary

One C++ process owns the gateway UDP socket, the bounded device state table, the command queue, the HomeKit bridge, and a small lock-presence monitor. The first cut excludes MQTT, Node.js, Homebridge, Home Assistant, ADB, cloud sessions, and scene CRUD.

## Gateway adapter

The adapter is responsible for validating the datagram envelope, device identity, message type, sequence, checksum, and decrypted body before it completes a command or changes a device state. Invalid, unknown, oversized, or stale packets must be discarded without growing memory use. UDP receive and HomeKit callbacks must not block on network retries.

The observed panel traffic uses UDP `192.0.2.12:80`, heartbeat command `0x0069`, heartbeat acknowledgement `0x03ed`, and state response `0x03ee`; its local UDP source port is ephemeral. The bridge uses AES-128-CBC encode/decode, outer and inner checksum checks, strict gateway identity, message-type validation, optional per-device sequence checks, and bounded JSON parsing. Its golden-vector tests cover heartbeat, state response, and the installed-firmware heartbeat-ACK type. The UDP socket returns raw datagrams; the runtime decodes and validates before changing state. Current automatic inventory sets every `sequence_key` to an empty string, so it does not provide per-device replay protection.

## State and command flow

- Catalog size and command queue capacity are fixed at startup.
- Device identity includes the gateway target and channel; sibling channels never share an unqualified power value.
- A HomeKit write is acknowledged after it enters the bounded command queue; a validated gateway reply is followed by a read-back that reconciles the cached state.
- A physical gateway report updates the cached characteristic and raises a HomeKit event only when its value changes.
- Cover position reports map to HomeKit Current Position; an explicit HomeKit target is tracked independently until the gateway reaches it, while external movement reports infer the endpoint from the direction.
- Gateway targets are queried at 250 ms spacing with a 1 s pause between complete rounds, keeping refresh latency bounded without increasing the per-request rate.
- Gateway heartbeats and device reports are handled separately. The runtime does not currently age out cached device states or expose a per-device availability characteristic.
- Unknown or unsupported device reports are discarded and are not retained; the runtime does not currently expose an unknown-target counter.

## HomeKit flow

The HAP server will run in bridge mode. A persistent, deterministic mapping assigns accessory IDs and characteristic IDs so a restart does not appear as a replacement set of accessories. Pairings, identity keys, and ID mappings are stored under a private data directory. Setup PINs and gateway keys never appear in logs.

The C++ layer owns accessory metadata and callbacks; the protocol implementation is delegated to the pinned HAP engine. The service runs in bridge mode with TCP port 53760, eight IP sessions, deterministic accessory IDs, a private key-value store, and mDNS advertisement through the control panel's DNS-SD service.

Panel inventory entries for radar/motion and water-leak devices are exposed as native HomeKit Occupancy Sensor and Leak Sensor services. Their read-only UInt8 characteristics follow validated gateway reports and raise HomeKit events when the reported state changes. Sensor accessory IDs remain stable when a previously exposed virtual-switch entry is promoted to the correct sensor service.

When configured, a HomeKit Occupancy Sensor reflects whether the lock responds on its configured LAN IP using its configured Wi-Fi MAC. IP and MAC are read from the root-only runtime file `config/lock-sensor.conf`, which is not compiled or packaged. A background monitor sends one ARP request for that exact IP every five seconds, checks the reply's IP and MAC, and marks it offline after 15 seconds without a match. It never enumerates or scans the subnet. This indicates LAN reachability, not an authenticated lock event or unlock state.

## Resource and failure limits

- No dynamic worker-per-device model; one event loop, one 64 KiB lock-monitor thread, and a small fixed number of HAP TCP sessions.
- Maximum packet, accessory, pending-command, and JSON/config sizes are explicit.
- Start with 32 bridged accessories and the HAP ADK minimum of eight IP sessions; do not use its sample default of 17 sessions.
- No retry of potentially non-idempotent gateway operations without a protocol-specific idempotency guarantee.
- A single malformed packet cannot terminate the receiver loop.
- A supervisor restarts an exited bridge with a bounded delay. A fixed-buffer writer rotates two private log files at a combined 10 MiB cap without recording raw frames or keys.

The gateway socket uses one connected UDP peer, an ephemeral local port, a fixed 8 KiB packet buffer, and an 8 KiB receive buffer. The configured HAP TCP port is separate from that gateway UDP port. The optional lock monitor opens a short-lived raw ARP socket only while probing the configured address.
