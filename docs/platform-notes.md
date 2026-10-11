# Platform Notes

**ESP32:** ESP-IDF builds lwIP PPP in client mode, so the peer has to assign the IP addresses (for example `local:remote` on the pppd command line). After the link drops, the library restarts the negotiation automatically. A TinyPPP instance takes the PPP slot of the Network library, so it can't be used together with the core's `PPP` (modem) library.

**RP2040 / RP2350:** the core's lwIP is built without `PPP_SUPPORT`, so TinyPPP uses its own small PPP engine:
- LCP with ACCM and magic number, accepting PFC and ACFC
- PAP client
- IPCP with address and DNS (RFC 1877) options
- Protocol-Reject for anything else (CCP, IPV6CP, ...)

The stream is polled by the core's lwIP background task (default every 20 ms). For more throughput, lower the period with `ppp.setPollingPeriod(2)`. On a UART, also make the RX buffer large enough, for example `Serial1.setFIFOSize(4096)`.

**Other 32-bit boards (generic):** these include the WiFiNINA boards (Nano 33 IoT, MKR WiFi 1010, Nano RP2040 Connect), UNO R4, Due and STM32. On these boards the TCP/IP stack runs inside the WiFi module or isn't present at all, so TinyPPP brings its own small one:
- TCP client and server with in-order receive, retransmission and zero-window probes, but no congestion control
- UDP, ICMP echo replies and a DNS resolver (A records)
- no IP fragmentation, no IPv6, no multicast

On WiFiNINA boards, `WiFiClient` and the other WiFiNINA classes keep using WiFi. Over PPP you use `PPPClient`, `PPPServer` and `PPPUDP`. There is no background task: every socket method processes the link. If the sketch doesn't use a socket for a while, call `ppp.loop()`.

The sizes can be changed with defines set before `#include <TinyPPP.h>` (defaults in brackets): `TINYPPP_TCP_SOCKETS` (4), `TINYPPP_TCP_RX_BUFFER` and `TINYPPP_TCP_TX_BUFFER` (1024 bytes per socket), `TINYPPP_TCP_LISTENERS` (2), `TINYPPP_UDP_SOCKETS` (2), `TINYPPP_UDP_RX_BUFFER` (1024) and `TINYPPP_UDP_TX_BUFFER` (576 bytes per `PPPUDP` object). With the defaults, TinyPPP uses about 13 KB of static RAM. See [RAM Requirements](ram.md) for the details.

**Generic stack on ESP32 and RP2040:** define `PPP_PREFER_GENERIC` before `#include <TinyPPP.h>` (or as a build flag) to use the generic implementation instead of the native one. `PPPClient`, `PPPServer` and `PPPUDP` are then the classes of the built-in stack. They are separate from the core's network classes, so `WiFiClient`, `NetworkClient` and `Network.onEvent()` don't work over PPP. As on the other generic boards, the link is processed by the socket methods and `ppp.loop()`; there is no background task.

The memory impact, measured with the `WebServer` example (flash and static RAM):

| Board | Native | `PPP_PREFER_GENERIC` |
|---|---|---|
| ESP32 | 439 KB flash, 27.3 KB RAM | 289 KB flash (−150 KB), 35.0 KB RAM (+7.7 KB) |
| Raspberry Pi Pico | 105 KB flash, 52.9 KB RAM | 70 KB flash (−35 KB), 21.6 KB RAM (−31 KB) |

On ESP32 the static RAM rises by the socket buffers of the generic stack. The native build also takes heap at runtime (the 4 KB reader task and lwIP buffers), which these numbers don't include. On RP2040 the savings come from not linking the core's lwIP and WiFi library. When the sketch uses WiFi as well, they are linked anyway and most of the savings are lost.

**Memory:** the engine uses about 1.8 KB per instance (a 1.5 KB receive frame buffer plus a 256 B transmit buffer). The buffers are on the hot path and small, so they stay in internal RAM. On ESP32, packet memory comes from lwIP and follows the core's PSRAM configuration.
