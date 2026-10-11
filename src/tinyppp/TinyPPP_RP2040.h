#pragma once
/**
 * RP2040 / RP2350 (Earle Philhower arduino-pico core) implementation.
 *
 * The core's lwIP is built without PPP support, so the TinyPPP engine is used
 * and registered as a point-to-point lwIP netif. All lwIP based Arduino
 * classes of the core (WiFiClient, WiFiServer, WiFiUDP, HTTPClient,
 * WebServer, ...) work over this interface.
 *
 * The stream is polled in the background by the lwIP_Ethernet packet handler
 * (see lwipPollingPeriod()), so no loop() call is needed.
 */
#include <Arduino.h>
#include <LwipEthernet.h>
#include <LwipIntf.h>
#include <WiFi.h>
#include <lwip/dns.h>
#include <lwip/ip4.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>

#include <functional>

#include "PPPEngine.h"

/// Portable aliases for the network classes that work over PPP
using PPPClient = WiFiClient;
using PPPServer = WiFiServer;
using PPPUDP = WiFiUDP;

extern std::function<void(struct netif *)> _addNetifCB;
extern std::function<void(struct netif *)> _removeNetifCB;

/**
 * @brief PPP network interface for RP2040 / RP2350 (arduino-pico): the
 * TinyPPP engine registered as a point-to-point lwIP netif.
 */
class TinyPPP : public LwipIntf, protected tinyppp::PPPEngine {
 public:
  TinyPPP() = default;
  TinyPPP(const TinyPPP &) = delete;
  TinyPPP &operator=(const TinyPPP &) = delete;
  ~TinyPPP() { end(); }

  /**
   * Starts PPP over the indicated stream (e.g. Serial, Serial1). The stream
   * must already be opened (Serial.begin()). Returns immediately: use
   * connected() or waitConnected() to wait for the link.
   */
  bool begin(Stream &io) {
    if (_netifAdded) end();
    _io = &io;

    ip4_addr_t any;
    ip4_addr_set_zero(&any);
    memset(&_netif, 0, sizeof(_netif));
    if (!netif_add(&_netif, &any, &any, &any, this, netifInit, ip4_input)) {
      return false;
    }
    _netifAdded = true;
    if (_addNetifCB) _addNetifCB(&_netif);

    // the engine is started from the lwIP context in the packet handler
    _cmd = Cmd::Start;
    _phID = __addEthernetPacketHandler([this] { this->poll(); });
    return true;
  }

  /// Terminates the link and removes the interface
  void end() {
    if (!_netifAdded) return;
    if (_phID >= 0) {
      _cmd = Cmd::Stop;
      // wait for the handler to send the LCP Terminate-Request
      uint32_t start = millis();
      while (_cmd != Cmd::None && millis() - start < 1000) delay(1);
      __removeEthernetPacketHandler(_phID);
      _phID = -1;
    }
    if (_removeNetifCB) _removeNetifCB(&_netif);
    netif_remove(&_netif);
    _netifAdded = false;
  }

  /**
   * Optional static configuration. local: our address (otherwise assigned by
   * the peer); peer: address assigned to the peer if it asks for one; dns1 and
   * dns2: DNS servers offered to the peer. Call before begin().
   */
  bool config(IPAddress local, IPAddress peer = IPAddress(0, 0, 0, 0),
              IPAddress dns1 = IPAddress(0, 0, 0, 0),
              IPAddress dns2 = IPAddress(0, 0, 0, 0)) {
    setLocalIP(tinyppp::fromIPAddress(local));
    setPeerIP(tinyppp::fromIPAddress(peer));
    setOfferedDNS(tinyppp::fromIPAddress(dns1), tinyppp::fromIPAddress(dns2));
    return true;
  }

  using PPPEngine::fcsErrors;
  using PPPEngine::phase;
  using PPPEngine::setCredentials;
  using PPPEngine::setEchoInterval;
  using PPPEngine::setPassive;
  using PPPEngine::setRequestDNS;
  using PPPEngine::stats;

  /// Debug output e.g. setLogger(Serial1)
  void setLogger(Print &out) { PPPEngine::setLogger(&out); }

  /// Make this interface the default route when the link is up (default true)
  void setDefault(bool flag = true) {
    _default = flag;
    if (flag && connected()) netif_set_default(&_netif);
  }

  /// Poll interval of the lwIP background task in ms (shared by all
  /// polled interfaces). Lower values give more throughput.
  void setPollingPeriod(int ms) { lwipPollingPeriod(ms); }

  /// Waits until the link is up
  bool waitConnected(uint32_t timeoutMs = 30000) {
    uint32_t start = millis();
    while (!connected()) {
      if (millis() - start >= timeoutMs) return false;
      delay(10);
    }
    return true;
  }

  /// true when the link is up and has an IP address
  bool connected() const { return _up; }
  /// WL_CONNECTED when the link is up, WL_NO_SHIELD before begin()
  wl_status_t status() const {
    if (!_netifAdded) return WL_NO_SHIELD;
    return _up ? WL_CONNECTED : WL_DISCONNECTED;
  }
  /// Our IP address
  IPAddress localIP() const { return IPAddress(ip4_addr_get_u32(netif_ip4_addr(&_netif))); }
  /// The IP address of the peer
  IPAddress gatewayIP() const { return IPAddress(ip4_addr_get_u32(netif_ip4_gw(&_netif))); }
  /// Always 255.255.255.255 (point-to-point link)
  IPAddress subnetMask() const { return IPAddress(ip4_addr_get_u32(netif_ip4_netmask(&_netif))); }
  /// The DNS server n (0 or 1) received from the peer
  IPAddress dnsIP(int n = 0) const {
    return tinyppp::toIPAddress(n == 0 ? dns1() : dns2());
  }

  /// Callback for link up/down
  using StateCallback = void (*)(bool up, void *ref);

  /// Callback for link up/down (called from the lwIP context); ref is passed
  /// to the callback
  void onStateChange(StateCallback cb, void *ref = nullptr) {
    _stateCallback = cb;
    _stateRef = ref;
  }

  /// The lwIP netif of the interface
  struct netif *getNetIf() { return &_netif; }

 protected:
  enum class Cmd : uint8_t { None, Start, Stop };

  struct netif _netif;
  Stream *_io = nullptr;
  int _phID = -1;
  volatile Cmd _cmd = Cmd::None;
  volatile bool _up = false;
  bool _netifAdded = false;
  bool _default = true;
  StateCallback _stateCallback = nullptr;
  void *_stateRef = nullptr;

  // called by the lwIP background task (lwIP lock held)
  void poll() {
    if (_cmd == Cmd::Start) {
      start(*_io);
      _cmd = Cmd::None;
    } else if (_cmd == Cmd::Stop) {
      stop();
      _cmd = Cmd::None;
      return;
    }
    process();
  }

  static err_t netifInit(struct netif *netif) {
    netif->name[0] = 'p';
    netif->name[1] = 'p';
    netif->mtu = 1500;
    netif->flags = 0;  // point-to-point: no ARP, no broadcast
    netif->output = output4;
#if LWIP_IPV6
    netif->output_ip6 = output6;
#endif
    return ERR_OK;
  }

  static err_t output4(struct netif *netif, struct pbuf *p,
                       const ip4_addr_t *addr) {
    (void)addr;
    TinyPPP *self = (TinyPPP *)netif->state;
    if (!self->beginIPv4()) return ERR_CONN;
    for (struct pbuf *q = p; q != nullptr; q = q->next) {
      self->writeIPv4((const uint8_t *)q->payload, q->len);
      if (q->len == q->tot_len) break;
    }
    return self->endIPv4() ? ERR_OK : ERR_IF;
  }

#if LWIP_IPV6
  static err_t output6(struct netif *, struct pbuf *, const ip6_addr_t *) {
    return ERR_IF;  // IPV6CP is not supported
  }
#endif

  void onIPv4Packet(const uint8_t *data, size_t len) override {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_POOL);
    if (p == nullptr) return;
    pbuf_take(p, data, (u16_t)len);
    if (_netif.input(p, &_netif) != ERR_OK) pbuf_free(p);
  }

  void onLinkUp() override {
    ip4_addr_t ip, mask, gw;
    ip4_addr_set_u32(&ip, PPPEngine::localIP());
    ip4_addr_set_u32(&gw, peerIP());
    ip4_addr_set_u32(&mask, 0xFFFFFFFF);
    netif_set_addr(&_netif, &ip, &mask, &gw);
    _netif.mtu = mtu();
    netif_set_link_up(&_netif);
    netif_set_up(&_netif);
    if (dns1() != 0) {
      ip_addr_t dns;
      ip_addr_set_ip4_u32(&dns, dns1());
      dns_setserver(0, &dns);
    }
    if (dns2() != 0) {
      ip_addr_t dns;
      ip_addr_set_ip4_u32(&dns, dns2());
      dns_setserver(1, &dns);
    }
    if (_default) netif_set_default(&_netif);
    _up = true;
    if (_stateCallback) _stateCallback(true, _stateRef);
  }

  void onLinkDown() override {
    _up = false;
    netif_set_down(&_netif);
    netif_set_link_down(&_netif);
    ip4_addr_t any;
    ip4_addr_set_zero(&any);
    netif_set_addr(&_netif, &any, &any, &any);
    if (_stateCallback) _stateCallback(false, _stateRef);
  }
};
