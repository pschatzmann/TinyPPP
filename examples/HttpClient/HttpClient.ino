/**
 * Access the internet over PPP via the USB serial port: the computer acts as
 * router (NAT). Debug output goes to Serial1.
 *
 * On the computer (Linux, replace eth0 by your internet interface):
 *   sudo sysctl -w net.ipv4.ip_forward=1
 *   sudo iptables -t nat -A POSTROUTING -o eth0 -j MASQUERADE
 *   sudo pppd /dev/ttyACM0 115200 192.168.7.1:192.168.7.2 noauth local \
 *        nodetach novj noccp nodefaultroute ms-dns 8.8.8.8
 *
 * Works on ESP32 (all variants) and RP2040 / RP2350.
 */
#include <TinyPPP.h>

TinyPPP ppp;

void setup() {
  Serial1.begin(115200);  // debug output
  Serial.begin(115200);   // PPP
  ppp.begin(Serial);

  Serial1.println("waiting for PPP connection...");
  while (!ppp.waitConnected(10000)) Serial1.println("still waiting...");
  Serial1.print("connected: ");
  Serial1.println(ppp.localIP());
}

void loop() {
  PPPClient client;
  if (client.connect("example.com", 80)) {
    client.print(
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n\r\n");
    uint32_t start = millis();
    while ((client.connected() || client.available()) &&
           millis() - start < 10000) {
      if (client.available()) {
        Serial1.write(client.read());
      } else {
        delay(1);
      }
    }
    client.stop();
  } else {
    Serial1.println("connect failed");
  }
  delay(10000);
}
