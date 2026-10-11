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

RP2040 and the desktop also offer the following:
- `config(local, peer, dns1, dns2)` sets static addresses. With it, the board can assign addresses to the peer (server role).
- `setPassive(bool)`
- `setEchoInterval(ms, failures)` enables LCP keep-alive.
- `onStateChange(cb)`
- `stats()`
- On RP2040 only: `setPollingPeriod(ms)`
