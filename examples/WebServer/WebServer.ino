/**
 * Simple web server reachable over PPP on the USB serial port.
 *
 * On the computer (Linux) start pppd on the serial device of the board:
 *   sudo pppd /dev/ttyACM0 115200 192.168.7.1:192.168.7.2 noauth local \
 *        nodetach novj noccp nodefaultroute
 * and open http://192.168.7.2 in the browser.
 *
 * Works on ESP32 (all variants), RP2040 / RP2350 and other 32-bit boards
 * (e.g. Nano 33 IoT, MKR WiFi 1010, Nano RP2040 Connect, UNO R4).
 */
#include <TinyPPP.h>

TinyPPP ppp;
PPPServer server(80);

void setup() {
  // PPP uses the USB serial: don't print anything else on it!
  Serial.begin(115200);
  ppp.begin(Serial);
  server.begin();
}

void loop() {
  PPPClient client = server.accept();
  if (!client) return;

  // skip the request header
  uint32_t start = millis();
  while (client.connected() && millis() - start < 2000) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 1) break;  // empty line: end of header
  }

  client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html\r\n"
      "Connection: close\r\n\r\n"
      "<html><body><h1>Hello from TinyPPP</h1><p>Uptime: ");
  client.print(millis() / 1000);
  client.print(" s</p><p>Local IP: ");
  client.print(ppp.localIP());
  client.println("</p></body></html>");
  client.stop();
}
