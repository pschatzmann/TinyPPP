#pragma once
/**
 * Generic implementation for platforms without lwIP: SAMD and mbed based
 * WiFiNINA boards (Nano 33 IoT, MKR WiFi 1010, Nano RP2040 Connect), the
 * desktop (Arduino-Emulator) and other cores.
 *
 * The PPP engine is combined with the small TinyPPP IPStack, so PPPClient,
 * PPPServer and PPPUDP work over the link. Received IPv4 packets that are not
 * handled by the stack can be processed with onReceive(), and raw packets can
 * be sent with send().
 *
 * There is no background task: the link is processed by all socket methods
 * and by loop(). Call loop() regularly when the sketch does not use a socket
 * for a while.
 */
#include "IPStack.h"
#include "PPPEngine.h"
#include "PPPSockets.h"

/**
 * @brief PPP network interface for platforms without lwIP: the PPP engine
 * combined with the TinyPPP IPStack (TCP, UDP, ICMP echo and DNS).
 */
class TinyPPP_Generic : public tinyppp::PPPEngine, public tinyppp::IPStack {
 public:
  /// Callback for received raw IPv4 packets
  using PacketCallback = void (*)(const uint8_t *data, size_t len, void *ref);
  /// Callback for link up/down
  using StateCallback = void (*)(bool up, void *ref);

  /// Starts the PPP negotiation over the indicated (already opened) stream
  bool begin(Stream &io) {
    IPStack::current() = this;
    start(io);
    return true;
  }

  /// Terminates the link
  void end() { stop(); }

  /// Processes input and timers: call regularly
  void loop() { poll(); }

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

  /// true when the link is up and has an IP address
  bool connected() const { return isUp(); }
  /// Our IP address
  IPAddress localIP() const { return tinyppp::toIPAddress(PPPEngine::localIP()); }
  /// The IP address of the peer
  IPAddress gatewayIP() const { return tinyppp::toIPAddress(peerIP()); }
  /// Always 255.255.255.255 (point-to-point link)
  IPAddress subnetMask() const { return IPAddress(255, 255, 255, 255); }
  /// The DNS server n (0 or 1) received from the peer
  IPAddress dnsIP(int n = 0) const {
    return tinyppp::toIPAddress(n == 0 ? dns1() : dns2());
  }

  /// Callback for received IPv4 packets that are not handled by the stack;
  /// ref is passed to the callback
  void onReceive(PacketCallback cb, void *ref = nullptr) {
    _rxCallback = cb;
    _rxRef = ref;
  }
  /// Callback for link up/down; ref is passed to the callback
  void onStateChange(StateCallback cb, void *ref = nullptr) {
    _stateCallback = cb;
    _stateRef = ref;
  }

  /// Sends a raw IPv4 packet
  bool send(const uint8_t *data, size_t len) { return sendIPv4(data, len); }

  using PPPEngine::setLogger;
  /// Debug output, e.g. setLogger(Serial1)
  void setLogger(Print &out) { PPPEngine::setLogger(&out); }

 protected:
  PacketCallback _rxCallback = nullptr;
  void *_rxRef = nullptr;
  StateCallback _stateCallback = nullptr;
  void *_stateRef = nullptr;

  void onIPv4Packet(const uint8_t *data, size_t len) override {
    // with a raw callback, unknown connections are not answered with resets
    if (ipInput(data, len, _rxCallback == nullptr)) return;
    if (_rxCallback) _rxCallback(data, len, _rxRef);
  }

  void onLinkUp() override {
    if (_stateCallback) _stateCallback(true, _stateRef);
  }

  void onLinkDown() override {
    ipLinkDown();
    if (_stateCallback) _stateCallback(false, _stateRef);
  }

  // IPStack link layer
  bool ipBegin() override { return beginIPv4(); }
  void ipWrite(const uint8_t *data, size_t len) override { writeIPv4(data, len); }
  bool ipEnd() override { return endIPv4(); }
  void ipPoll() override { process(); }
  bool ipIsUp() const override { return isUp(); }
  tinyppp::ip4_t ipLocal() const override { return PPPEngine::localIP(); }
  tinyppp::ip4_t ipDns(int n) const override { return n == 0 ? dns1() : dns2(); }
  uint16_t ipMtu() const override {
    uint16_t m = mtu();
    return m > TINYPPP_MRU ? TINYPPP_MRU : m;
  }
};

/// The PPP network interface of this platform
using TinyPPP = TinyPPP_Generic;
