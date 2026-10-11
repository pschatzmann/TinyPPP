# RAM Requirements of the Generic Stack

On the generic platforms, all memory is static (no heap). Static RAM used by TinyPPP:

| Part | RAM |
|---|---|
| PPP engine, IP stack and DNS state | about 2.5 KB |
| Each TCP socket (`TINYPPP_TCP_SOCKETS`, default 4) | `TINYPPP_TCP_RX_BUFFER` + `TINYPPP_TCP_TX_BUFFER` + about 70 B (default 2.1 KB) |
| Each UDP socket (`TINYPPP_UDP_SOCKETS`, default 2) | `TINYPPP_UDP_RX_BUFFER` + about 20 B (default 1 KB) |
| Each `PPPUDP` object | `TINYPPP_UDP_TX_BUFFER` + about 30 B (default 0.6 KB) |

Measured on a Nano 33 IoT (SAMD21, 32 KB RAM):

| Configuration | TinyPPP static RAM |
|---|---|
| Defaults: 4 TCP sockets with 1024 B buffers, 2 UDP sockets with 1024 B buffers | 13.0 KB |
| 2 TCP sockets with 512 B buffers, 1 UDP socket with 512 B buffer | 5.2 KB |

On top of this, the stack needs about 1 KB of stack space while it processes packets, and `hostByName()` needs about 0.3 KB more. To use less RAM, set smaller values before including the library:

```cpp
#define TINYPPP_TCP_SOCKETS 2
#define TINYPPP_TCP_RX_BUFFER 512
#define TINYPPP_TCP_TX_BUFFER 512
#define TINYPPP_UDP_SOCKETS 1
#define TINYPPP_UDP_RX_BUFFER 512
#include <TinyPPP.h>
```

The TCP receive buffer limits the window that is offered to the peer. Small buffers therefore lower the throughput, but the link keeps working. The defines only take effect when they come before every `#include <TinyPPP.h>`, so in a multi-file sketch it's better to set them as build flags.

The generic stack can also be used on ESP32 and RP2040 with `PPP_PREFER_GENERIC`. See [Platform Notes](platform-notes.md) for the memory comparison with the native implementations.
