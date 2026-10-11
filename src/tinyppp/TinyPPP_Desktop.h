#pragma once
/**
 * Desktop (Arduino-Emulator) and generic fallback implementation: the PPP
 * engine without an IP stack. Received IPv4 packets are delivered to a
 * callback and packets can be sent with send(). Optionally answers ICMP echo
 * requests so that the link can be tested with ping.
 *
 * There is no background task: call loop() regularly.
 */
#include <functional>

#include "PPPEngine.h"

class TinyPPP : public tinyppp::PPPEngine {
 public:
  using PacketCallback = std::function<void(const uint8_t *, size_t)>;

  /// Starts the PPP negotiation over the indicated (already opened) stream
  bool begin(Stream &io) {
    start(io);
    return true;
  }

  void end() { stop(); }

  /// Processes input and timers: call regularly
  void loop() { process(); }

  /// Static configuration: our address, the peer address and the DNS servers
  /// we offer to the peer. Unset values are negotiated.
  bool config(IPAddress local, IPAddress peer = IPAddress(0, 0, 0, 0),
              IPAddress dns1 = IPAddress(0, 0, 0, 0),
              IPAddress dns2 = IPAddress(0, 0, 0, 0)) {
    setLocalIP(tinyppp::fromIPAddress(local));
    setPeerIP(tinyppp::fromIPAddress(peer));
    setOfferedDNS(tinyppp::fromIPAddress(dns1), tinyppp::fromIPAddress(dns2));
    return true;
  }

  /// Waits (while processing the link) until IPCP is up
  bool waitConnected(uint32_t timeoutMs = 30000) {
    uint32_t start = millis();
    while (!connected()) {
      if (millis() - start >= timeoutMs) return false;
      loop();
      delay(1);
    }
    return true;
  }

  bool connected() const { return isUp(); }
  IPAddress localIP() const { return tinyppp::toIPAddress(PPPEngine::localIP()); }
  IPAddress gatewayIP() const { return tinyppp::toIPAddress(peerIP()); }
  IPAddress subnetMask() const { return IPAddress(255, 255, 255, 255); }
  IPAddress dnsIP(int n = 0) const {
    return tinyppp::toIPAddress(n == 0 ? dns1() : dns2());
  }

  /// Callback for received IPv4 packets
  void onReceive(PacketCallback cb) { _rxCallback = cb; }
  /// Callback for link up/down
  void onStateChange(std::function<void(bool up)> cb) { _stateCallback = cb; }

  /// Sends an IPv4 packet
  bool send(const uint8_t *data, size_t len) { return sendIPv4(data, len); }

  /// Debug output, e.g. setLogger(Serial)
  using PPPEngine::setLogger;
  void setLogger(Print &out) { PPPEngine::setLogger(&out); }

  /// Answer ICMP echo requests addressed to us (default true)
  void setRespondToPing(bool flag) { _ping = flag; }

 protected:
  PacketCallback _rxCallback;
  std::function<void(bool)> _stateCallback;
  bool _ping = true;

  void onIPv4Packet(const uint8_t *data, size_t len) override {
    if (_ping && replyToPing(data, len)) return;
    if (_rxCallback) _rxCallback(data, len);
  }

  void onLinkUp() override {
    if (_stateCallback) _stateCallback(true);
  }

  void onLinkDown() override {
    if (_stateCallback) _stateCallback(false);
  }

  static uint16_t checksum(const uint8_t *p, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
  }

  bool replyToPing(const uint8_t *data, size_t len) {
    if (len < 28 || (data[0] >> 4) != 4) return false;
    size_t ihl = (data[0] & 0x0F) * 4;
    size_t total = (size_t)(data[2] << 8 | data[3]);
    if (ihl < 20 || total > len || total < ihl + 8) return false;
    if (data[9] != 1 || data[ihl] != 8) return false;  // ICMP echo request
    tinyppp::ip4_t dst;
    memcpy(&dst, data + 16, 4);
    if (dst != PPPEngine::localIP()) return false;
    if (total > TINYPPP_MRU) return true;
    uint8_t reply[TINYPPP_MRU];
    memcpy(reply, data, total);
    // swap addresses
    memcpy(reply + 12, data + 16, 4);
    memcpy(reply + 16, data + 12, 4);
    reply[8] = 64;  // ttl
    reply[10] = reply[11] = 0;
    uint16_t c = checksum(reply, ihl);
    reply[10] = c >> 8;
    reply[11] = c & 0xFF;
    // ICMP echo reply
    uint8_t *icmp = reply + ihl;
    icmp[0] = 0;
    icmp[2] = icmp[3] = 0;
    c = checksum(icmp, total - ihl);
    icmp[2] = c >> 8;
    icmp[3] = c & 0xFF;
    sendIPv4(reply, total);
    return true;
  }
};
