# Initial memory budget

The target has about 2 GiB of RAM, so the design avoids a broker, scripting engine, and per-device threads. The gateway path uses one fixed 8 KiB packet buffer and requests an 8 KiB UDP receive buffer, matching the codec's maximum frame size. Accessory metadata is capped at 32 by default; the fixed ID/name buffers are allocated at startup, and the state table does not grow after startup. The optional lock monitor uses one 64 KiB thread and one targeted ARP request per interval; it does not enumerate LAN hosts.

The pinned HAP ADK requires at least eight IP sessions. Its default inbound and outbound buffers are each 32 KiB per session, with a separate 32 KiB scratch buffer. Using eight sessions gives about 544 KiB for those buffers before the accessory event table and protocol state. That is a conservative initial allocation; reducing these buffers requires an interoperability test with a full accessory-list read and multiple paired controllers.

The target is under 24 MiB at idle and under 32 MiB during pairing or a full accessory-list read. These remain acceptance targets for stress testing.

After deploying v1.1.3 on 2026-10-16, the live panel reported 6,628 kB RSS and 6,744 kB peak-since-start RSS for the bridge, and 2,092 kB RSS and 2,116 kB peak-since-start RSS for the log writer. The panel reported 1,146,696 kB `MemAvailable`. These `/proc` readings are a single snapshot shortly after startup and a doctor run; `VmHWM` records each process's high-water mark since it started, but this was not a controlled pairing or concurrent accessory-list stress test. The maintenance preflight prints the process RSS and high-water mark for both service processes.
