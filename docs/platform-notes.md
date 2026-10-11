# Platform Notes

**ESP32:** ESP-IDF builds lwIP PPP in client mode, so the peer has to assign the IP addresses (for example `local:remote` on the pppd command line). After the link drops, the library restarts the negotiation automatically. A TinyPPP instance takes the PPP slot of the Network library, so it can't be used together with the core's `PPP` (modem) library.

**RP2040 / RP2350:** the core's lwIP is built without `PPP_SUPPORT`, so TinyPPP uses its own small PPP engine:
- LCP with ACCM and magic number, accepting PFC and ACFC
- PAP client
- IPCP with address and DNS (RFC 1877) options
- Protocol-Reject for anything else (CCP, IPV6CP, ...)

The stream is polled by the core's lwIP background task (default every 20 ms). For more throughput, lower the period with `ppp.setPollingPeriod(2)`. On a UART, also make the RX buffer large enough, for example `Serial1.setFIFOSize(4096)`.

**Memory:** the engine uses about 1.8 KB per instance (a 1.5 KB receive frame buffer plus a 256 B transmit buffer). The buffers are on the hot path and small, so they stay in internal RAM. On ESP32, packet memory comes from lwIP and follows the core's PSRAM configuration.
