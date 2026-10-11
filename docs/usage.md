# Usage

```cpp
#include <TinyPPP.h>

TinyPPP ppp;
PPPServer server(80);

void setup() {
  Serial.begin(115200);  // the USB serial carries PPP: don't print on it!
  ppp.begin(Serial);     // returns at once, the link comes up in the background
  server.begin();
}

void loop() {
  PPPClient client = server.accept();
  if (client) {
    client.print("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nHello");
    client.stop();
  }
}
```

## API

API on all platforms: `begin(Stream&)`, `end()`, `connected()`, `waitConnected(ms)`, `localIP()`, `gatewayIP()`, `subnetMask()`, `dnsIP(n)`, `setCredentials(user, password)` (PAP) and `setLogger(Print&)`.

RP2040 and the generic platforms (the other 32-bit boards and the desktop) also offer the following:
- `config(local, peer, dns1, dns2)` sets static addresses. With it, the board can assign addresses to the peer (server role).
- `setPassive(bool)`
- `setEchoInterval(ms, failures)` enables LCP keep-alive.
- `onStateChange(cb, ref)`: `cb` is a plain function `void cb(bool up, void *ref)`, and `ref` is passed through to it.
- `stats()`
- On RP2040 only: `setPollingPeriod(ms)`

On the generic platforms only:
- `loop()` processes the link. Every `PPPClient`, `PPPServer` and `PPPUDP` method also does this, so you only need `loop()` when the sketch goes a while without using a socket.
- `hostByName(host, ip)` resolves a name through the DNS server received from the peer.
- `onReceive(cb, ref)` receives the raw IPv4 packets that the stack doesn't handle. `send(data, len)` sends a raw packet.
- `onIdle(cb, ref)` is called while a socket method waits.
