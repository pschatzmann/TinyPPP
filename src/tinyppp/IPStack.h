#pragma once
/**
 * A minimal IPv4 stack for platforms without lwIP (e.g. SAMD and mbed based
 * WiFiNINA boards, the desktop): ICMP echo, UDP, TCP and a DNS resolver. It
 * is independent from PPP: the link layer implements ipBegin(), ipWrite(),
 * ipEnd() and ipPoll() and passes received packets to ipInput().
 *
 * Limitations (by design, to keep it small):
 * - TCP accepts in-order segments only and has no congestion control
 * - no IP fragmentation and reassembly, no IP options on output
 * - no IPv6, no multicast
 *
 * There is no background task: the stack is processed in ipPoll(), which is
 * called by all socket methods. Call it regularly (e.g. TinyPPP::loop()) when
 * the sketch is busy with something else.
 */
#include <stdint.h>
#include <string.h>

#include <Arduino.h>

#ifndef TINYPPP_TCP_SOCKETS
#define TINYPPP_TCP_SOCKETS 4
#endif

#ifndef TINYPPP_TCP_RX_BUFFER
#define TINYPPP_TCP_RX_BUFFER 1024
#endif

#ifndef TINYPPP_TCP_TX_BUFFER
#define TINYPPP_TCP_TX_BUFFER 1024
#endif

#ifndef TINYPPP_TCP_LISTENERS
#define TINYPPP_TCP_LISTENERS 2
#endif

#ifndef TINYPPP_UDP_SOCKETS
#define TINYPPP_UDP_SOCKETS 2
#endif

#ifndef TINYPPP_UDP_RX_BUFFER
#define TINYPPP_UDP_RX_BUFFER 1024
#endif

#ifndef TINYPPP_UDP_TX_BUFFER
#define TINYPPP_UDP_TX_BUFFER 576
#endif

/// Initial TCP retransmission timeout in ms (doubled on each retry)
#ifndef TINYPPP_TCP_RTO
#define TINYPPP_TCP_RTO 1000
#endif

#ifndef TINYPPP_TCP_MAX_RETRIES
#define TINYPPP_TCP_MAX_RETRIES 8
#endif

/// Time in ms a closed socket stays in TIME_WAIT
#ifndef TINYPPP_TCP_TIME_WAIT
#define TINYPPP_TCP_TIME_WAIT 2000
#endif

/// Max time in ms a socket closed by stop() is kept to complete the close
#ifndef TINYPPP_TCP_LINGER
#define TINYPPP_TCP_LINGER 10000
#endif

namespace tinyppp {

/// IPv4 address in network byte order as stored in memory
using ip4_t = uint32_t;

/// @brief Byte ring buffer with a fixed capacity of N bytes
template <size_t N>
class RingBuffer {
 public:
  /// Number of stored bytes
  size_t available() const { return _len; }
  /// Free space in bytes
  size_t free() const { return N - _len; }
  /// Capacity in bytes
  size_t size() const { return N; }
  /// Removes all data
  void clear() { _head = _len = 0; }

  /// Appends data: returns the number of bytes stored
  size_t write(const uint8_t *data, size_t len) {
    if (len > free()) len = free();
    for (size_t i = 0; i < len; i++) _buf[(_head + _len + i) % N] = data[i];
    _len += len;
    return len;
  }

  /// Removes and returns one byte (-1 if empty)
  int read() {
    if (_len == 0) return -1;
    uint8_t b = _buf[_head];
    discard(1);
    return b;
  }

  /// Removes up to len bytes into data
  size_t read(uint8_t *data, size_t len) {
    len = peek(0, data, len);
    discard(len);
    return len;
  }

  /// Returns the next byte without removing it (-1 if empty)
  int peek() const { return _len == 0 ? -1 : _buf[_head]; }

  /// Copies data at offset without removing it
  size_t peek(size_t offset, uint8_t *data, size_t len) const {
    if (offset >= _len) return 0;
    if (len > _len - offset) len = _len - offset;
    for (size_t i = 0; i < len; i++) data[i] = _buf[(_head + offset + i) % N];
    return len;
  }

  /// Contiguous data at offset (max len): returns the number of bytes at p
  size_t contiguous(size_t offset, size_t len, const uint8_t *&p) const {
    if (offset >= _len) return 0;
    if (len > _len - offset) len = _len - offset;
    size_t pos = (_head + offset) % N;
    if (len > N - pos) len = N - pos;
    p = _buf + pos;
    return len;
  }

  /// Removes up to len bytes
  void discard(size_t len) {
    if (len > _len) len = _len;
    _head = (_head + len) % N;
    _len -= len;
    if (_len == 0) _head = 0;
  }

 private:
  uint8_t _buf[N];
  size_t _head = 0;
  size_t _len = 0;
};

/// @brief Internet checksum (RFC 1071) over several (possibly odd sized)
/// pieces
class Checksum {
 public:
  /// Adds data
  void add(const uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
      _sum += _odd ? p[i] : (uint32_t)p[i] << 8;
      _odd = !_odd;
    }
  }
  /// Adds a 16 bit value
  void add16(uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    add(b, 2);
  }
  /// The checksum of all added data
  uint16_t result() const {
    uint32_t s = _sum;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
  }

 private:
  uint32_t _sum = 0;
  bool _odd = false;
};

/// TCP connection states (RFC 793)
enum class TCPState : uint8_t {
  Closed,
  SynSent,
  SynRcvd,
  Established,
  FinWait1,
  FinWait2,
  CloseWait,
  Closing,
  LastAck,
  TimeWait,
};

/**
 * @brief Minimal IPv4 stack (ICMP echo, UDP, TCP, DNS resolver) for
 * platforms without lwIP.
 *
 * The link layer (e.g. TinyPPP) derives from this class and implements the
 * ip* methods. The socket methods are used by PPPClient, PPPServer and
 * PPPUDP; sockets are identified by their index in the socket table.
 */
class IPStack {
 public:
  static constexpr uint8_t IP_ICMP = 1;
  static constexpr uint8_t IP_TCP = 6;
  static constexpr uint8_t IP_UDP = 17;

  static constexpr uint8_t TCP_FIN = 0x01;
  static constexpr uint8_t TCP_SYN = 0x02;
  static constexpr uint8_t TCP_RST = 0x04;
  static constexpr uint8_t TCP_PSH = 0x08;
  static constexpr uint8_t TCP_ACK = 0x10;

  IPStack() {
    if (current() == nullptr) current() = this;
  }
  virtual ~IPStack() {
    if (current() == this) current() = nullptr;
  }

  /// The stack used by sockets that were created without an explicit stack
  static IPStack *&current() {
    static IPStack *stack = nullptr;
    return stack;
  }

  /// Processes the link (input and timers): called by all socket methods
  void poll() {
    if (_inPoll) return;
    _inPoll = true;
    ipPoll();
    processTimers();
    flushAcks();
    _inPoll = false;
  }

  /// Callback that is called while a socket method waits
  using IdleCallback = void (*)(void *ref);

  /// Called while a socket method waits (e.g. to run other tasks); ref is
  /// passed to the callback
  void onIdle(IdleCallback cb, void *ref = nullptr) {
    _idle = cb;
    _idleRef = ref;
  }

  /// Answer ICMP echo requests addressed to us (default true)
  void setRespondToPing(bool flag) { _ping = flag; }

  /// Resolves a host name (or a dotted IP address) via DNS
  bool hostByName(const char *host, IPAddress &result,
                  uint32_t timeoutMs = 5000) {
    if (result.fromString(host)) return true;
    if (!ipIsUp()) return false;
    for (int n = 0; n < 2; n++) {
      ip4_t server = ipDns(n);
      if (server == 0) continue;
      if (dnsQuery(server, host, timeoutMs / 2, result)) return true;
    }
    return false;
  }

  // ---- TCP socket API (used by PPPClient / PPPServer) ----

  /// Opens a connection: returns the socket number or -1
  int tcpConnect(ip4_t ip, uint16_t port, uint32_t timeoutMs) {
    poll();
    if (!ipIsUp()) return -1;
    int s = allocTcp();
    if (s < 0) return -1;
    TCPSocket &t = _tcp[s];
    t.remoteIP = ip;
    t.remotePort = port;
    t.localPort = ephemeralPort();
    t.state = TCPState::SynSent;
    t.accepted = true;
    sendSyn(t);
    uint32_t start = millis();
    while (t.state == TCPState::SynSent) {
      if (millis() - start >= timeoutMs) {
        sendReset(t);
        freeTcp(t);
        return -1;
      }
      idle();
    }
    if (t.state != TCPState::Established && t.state != TCPState::CloseWait) {
      freeTcp(t);
      return -1;
    }
    return s;
  }

  /// true if socket s is open and still has the generation gen
  bool tcpValid(int s, uint8_t gen) const {
    return s >= 0 && s < TINYPPP_TCP_SOCKETS && _tcp[s].inUse &&
           !_tcp[s].orphan && _tcp[s].gen == gen;
  }
  /// Generation of socket s: changes each time the socket is reused
  uint8_t tcpGen(int s) const { return _tcp[s].gen; }
  /// Connection state of socket s
  TCPState tcpState(int s) const { return _tcp[s].state; }
  /// Local port of socket s
  uint16_t tcpLocalPort(int s) const { return _tcp[s].localPort; }
  /// Remote port of socket s
  uint16_t tcpRemotePort(int s) const { return _tcp[s].remotePort; }
  /// Remote address of socket s
  ip4_t tcpRemoteIP(int s) const { return _tcp[s].remoteIP; }

  /// Number of received bytes that can be read from socket s
  int tcpAvailable(int s) {
    poll();
    return (int)_tcp[s].rx.available();
  }

  /// Next received byte of socket s without removing it (-1 if none)
  int tcpPeek(int s) {
    poll();
    return _tcp[s].rx.peek();
  }

  /// Reads up to len received bytes from socket s
  int tcpRead(int s, uint8_t *data, size_t len) {
    poll();
    TCPSocket &t = _tcp[s];
    size_t n = t.rx.read(data, len);
    if (n > 0) windowUpdate(t);
    return (int)n;
  }

  /// Queues data for sending (waits for buffer space up to timeoutMs)
  size_t tcpWrite(int s, const uint8_t *data, size_t len, uint32_t timeoutMs) {
    TCPSocket &t = _tcp[s];
    uint8_t gen = t.gen;
    size_t done = 0;
    uint32_t start = millis();
    while (done < len) {
      poll();
      if (!t.inUse || t.gen != gen || !canSend(t)) break;
      size_t n = t.tx.write(data + done, len - done);
      if (n > 0) {
        done += n;
        output(t);
        start = millis();
      } else {
        if (millis() - start >= timeoutMs) break;
        idle();
      }
    }
    return done;
  }

  /// Waits until all queued data was acknowledged
  void tcpFlush(int s, uint32_t timeoutMs) {
    TCPSocket &t = _tcp[s];
    uint8_t gen = t.gen;
    uint32_t start = millis();
    while (t.inUse && t.gen == gen && t.tx.available() > 0 && canSend(t) &&
           millis() - start < timeoutMs) {
      idle();
    }
  }

  /// Closes the connection: waits up to timeoutMs for the close handshake,
  /// the rest is completed in the background.
  void tcpClose(int s, uint32_t timeoutMs) {
    TCPSocket &t = _tcp[s];
    uint8_t gen = t.gen;
    t.rx.clear();
    switch (t.state) {
      case TCPState::Established:
      case TCPState::CloseWait:
        t.finQueued = true;
        output(t);
        break;
      case TCPState::SynSent:
      case TCPState::SynRcvd:
        sendReset(t);
        freeTcp(t);
        return;
      case TCPState::Closed:
        freeTcp(t);
        return;
      default:
        break;
    }
    uint32_t start = millis();
    while (t.inUse && t.gen == gen && millis() - start < timeoutMs &&
           t.state != TCPState::Closed && t.state != TCPState::TimeWait &&
           t.state != TCPState::FinWait2) {
      idle();
    }
    if (!t.inUse || t.gen != gen) return;
    if (t.state == TCPState::Closed) {
      freeTcp(t);
      return;
    }
    t.orphan = true;
    if (t.state != TCPState::TimeWait) t.deadline = millis() + TINYPPP_TCP_LINGER;
  }

  /// Starts listening on a port
  bool tcpListen(uint16_t port) {
    for (auto &l : _listeners)
      if (l == port) return true;
    for (auto &l : _listeners) {
      if (l == 0) {
        l = port;
        return true;
      }
    }
    return false;
  }

  /// Stops listening on a port
  void tcpUnlisten(uint16_t port) {
    for (auto &l : _listeners)
      if (l == port) l = 0;
  }

  /// Returns a new connection on the port or -1. With withData only
  /// connections that have data are returned (also already accepted ones).
  int tcpAccept(uint16_t port, bool withData) {
    poll();
    for (int i = 0; i < TINYPPP_TCP_SOCKETS; i++) {
      TCPSocket &t = _tcp[i];
      if (!t.inUse || t.orphan || t.localPort != port) continue;
      if (t.state == TCPState::SynRcvd || t.state == TCPState::SynSent) continue;
      if (withData) {
        if (t.rx.available() == 0) continue;
      } else if (t.accepted) {
        continue;
      }
      t.accepted = true;
      return i;
    }
    return -1;
  }

  /// Sends data to all connected clients of a server port
  size_t tcpWriteAll(uint16_t port, const uint8_t *data, size_t len,
                     uint32_t timeoutMs) {
    size_t result = 0;
    for (int i = 0; i < TINYPPP_TCP_SOCKETS; i++) {
      TCPSocket &t = _tcp[i];
      if (t.inUse && !t.orphan && t.accepted && t.localPort == port &&
          canSend(t))
        result = tcpWrite(i, data, len, timeoutMs);
    }
    return result;
  }

  // ---- UDP socket API (used by PPPUDP) ----

  /// Opens a UDP socket on the port (0: ephemeral): returns it or -1
  int udpOpen(uint16_t port) {
    if (port == 0) port = ephemeralPort();
    for (int i = 0; i < TINYPPP_UDP_SOCKETS; i++) {
      if (_udp[i].inUse && _udp[i].localPort == port) return -1;
    }
    for (int i = 0; i < TINYPPP_UDP_SOCKETS; i++) {
      UDPSocket &u = _udp[i];
      if (!u.inUse) {
        u.inUse = true;
        u.localPort = port;
        u.rx.clear();
        u.rxRemaining = 0;
        return i;
      }
    }
    return -1;
  }

  /// Closes UDP socket s
  void udpClose(int s) {
    _udp[s].inUse = false;
    _udp[s].rx.clear();
    _udp[s].rxRemaining = 0;
  }

  /// Local port of UDP socket s
  uint16_t udpLocalPort(int s) const { return _udp[s].localPort; }

  /// Sends a datagram
  bool udpSend(int s, ip4_t ip, uint16_t port, const uint8_t *data,
               size_t len) {
    poll();
    return sendUdp(_udp[s].localPort, ip, port, data, len);
  }

  /// Moves to the next datagram: returns its size (0 if none)
  int udpParsePacket(int s, ip4_t &ip, uint16_t &port) {
    poll();
    UDPSocket &u = _udp[s];
    u.rx.discard(u.rxRemaining);
    u.rxRemaining = 0;
    if (u.rx.available() < 8) return 0;
    uint8_t hdr[8];
    u.rx.read(hdr, 8);
    memcpy(&ip, hdr, 4);
    port = get16(hdr + 4);
    u.rxRemaining = get16(hdr + 6);
    return (int)u.rxRemaining;
  }

  /// Unread bytes of the current datagram
  int udpAvailable(int s) const { return (int)_udp[s].rxRemaining; }

  /// Reads from the current datagram
  int udpRead(int s, uint8_t *data, size_t len) {
    UDPSocket &u = _udp[s];
    if (len > u.rxRemaining) len = u.rxRemaining;
    size_t n = u.rx.read(data, len);
    u.rxRemaining -= n;
    return (int)n;
  }

  /// Next byte of the current datagram without removing it (-1 if none)
  int udpPeek(int s) const {
    return _udp[s].rxRemaining > 0 ? _udp[s].rx.peek() : -1;
  }

  /// Discards the rest of the current datagram
  void udpDiscard(int s) {
    UDPSocket &u = _udp[s];
    u.rx.discard(u.rxRemaining);
    u.rxRemaining = 0;
  }

 protected:
  // ---- interface to the link layer ----

  /// Starts an outgoing IPv4 packet
  virtual bool ipBegin() = 0;
  /// Adds data to the outgoing packet
  virtual void ipWrite(const uint8_t *data, size_t len) = 0;
  /// Sends the packet
  virtual bool ipEnd() = 0;
  /// Processes link input: received packets are passed to ipInput()
  virtual void ipPoll() = 0;
  virtual bool ipIsUp() const = 0;
  virtual ip4_t ipLocal() const = 0;
  virtual ip4_t ipDns(int n) const = 0;
  virtual uint16_t ipMtu() const = 0;

  /// Processes a received IPv4 packet: returns false if it was not
  /// consumed by the stack (the caller can handle it). With reply set to
  /// false, unconsumed packets are not answered with TCP reset.
  bool ipInput(const uint8_t *data, size_t len, bool reply = true) {
    if (len < 20 || (data[0] >> 4) != 4) return false;
    size_t ihl = (data[0] & 0x0F) * 4;
    size_t total = get16(data + 2);
    if (ihl < 20 || total > len || total < ihl) return false;
    ip4_t dst;
    memcpy(&dst, data + 16, 4);
    if (dst != ipLocal()) return false;
    if ((get16(data + 6) & 0x3FFF) != 0) return false;  // fragment
    Checksum c;
    c.add(data, ihl);
    if (c.result() != 0) return false;
    ip4_t src;
    memcpy(&src, data + 12, 4);
    const uint8_t *payload = data + ihl;
    size_t plen = total - ihl;
    switch (data[9]) {
      case IP_ICMP:
        return icmpInput(data, total, ihl);
      case IP_UDP:
        return udpInput(src, dst, payload, plen);
      case IP_TCP:
        return tcpInput(src, dst, payload, plen, reply);
      default:
        return false;
    }
  }

  /// Called when the link goes down: all connections are reset
  void ipLinkDown() {
    for (auto &t : _tcp) {
      if (!t.inUse) continue;
      t.state = TCPState::Closed;
      t.timer = 0;
      if (t.orphan || !t.accepted) freeTcp(t);
    }
  }

 private:
  struct TCPSocket {
    bool inUse = false;
    bool orphan = false;    // closed by the user, finishing in background
    bool accepted = false;  // handed out to the user
    bool ackPending = false;
    bool finQueued = false;  // send FIN after the queued data
    bool finSent = false;
    uint8_t gen = 0;
    uint8_t retries = 0;
    TCPState state = TCPState::Closed;
    uint16_t localPort = 0;
    uint16_t remotePort = 0;
    ip4_t remoteIP = 0;
    uint32_t iss = 0;
    uint32_t sndUna = 0;
    uint32_t sndNxt = 0;
    uint32_t rcvNxt = 0;
    uint32_t sndWnd = 0;
    uint16_t mss = 536;
    uint16_t advWnd = 0;  // last advertised window
    uint32_t rto = TINYPPP_TCP_RTO;
    uint32_t timer = 0;     // retransmission deadline, 0 = stopped
    uint32_t deadline = 0;  // TIME_WAIT / linger deadline, 0 = none
    RingBuffer<TINYPPP_TCP_RX_BUFFER> rx;
    RingBuffer<TINYPPP_TCP_TX_BUFFER> tx;  // unacknowledged and unsent data
  };

  struct UDPSocket {
    bool inUse = false;
    uint16_t localPort = 0;
    size_t rxRemaining = 0;
    // datagrams: ip(4) port(2) len(2) data
    RingBuffer<TINYPPP_UDP_RX_BUFFER> rx;
  };

  TCPSocket _tcp[TINYPPP_TCP_SOCKETS];
  UDPSocket _udp[TINYPPP_UDP_SOCKETS];
  uint16_t _listeners[TINYPPP_TCP_LISTENERS] = {0};
  uint16_t _nextPort = 0;
  uint16_t _ipId = 0;
  bool _ping = true;
  bool _inPoll = false;
  IdleCallback _idle = nullptr;
  void *_idleRef = nullptr;

  // DNS request in progress
  uint16_t _dnsPort = 0;
  uint16_t _dnsId = 0;
  bool _dnsDone = false;
  ip4_t _dnsResult = 0;

  static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
  }
  static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           p[3];
  }
  static void put16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
  }
  static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
  }
  // sequence number comparison
  static bool seqLT(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
  static bool seqGT(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

  void idle() {
    if (_idle) _idle(_idleRef);
    poll();
    delay(1);
  }

  uint16_t ephemeralPort() {
    if (_nextPort == 0) _nextPort = 49152 + (uint16_t)(random(16000));
    for (int tries = 0; tries < 100; tries++) {
      uint16_t p = _nextPort++;
      if (_nextPort < 49152) _nextPort = 49152;
      if (!portInUse(p)) return p;
    }
    return _nextPort;
  }

  bool portInUse(uint16_t port) const {
    for (const auto &t : _tcp)
      if (t.inUse && t.localPort == port) return true;
    for (const auto &u : _udp)
      if (u.inUse && u.localPort == port) return true;
    return port == _dnsPort;
  }

  bool listening(uint16_t port) const {
    for (auto l : _listeners)
      if (l != 0 && l == port) return true;
    return false;
  }

  // ---- IP output ----

  /// Sends an IP packet with the indicated protocol header (checksum
  /// already set) and up to two payload pieces
  bool sendIP(uint8_t proto, ip4_t dst, const uint8_t *hdr, size_t hdrLen,
              const uint8_t *d1, size_t n1, const uint8_t *d2 = nullptr,
              size_t n2 = 0) {
    uint8_t ip[20];
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(20 + hdrLen + n1 + n2));
    put16(ip + 4, _ipId++);
    put16(ip + 6, 0x4000);  // don't fragment
    ip[8] = 64;
    ip[9] = proto;
    ip[10] = ip[11] = 0;
    ip4_t src = ipLocal();
    memcpy(ip + 12, &src, 4);
    memcpy(ip + 16, &dst, 4);
    Checksum c;
    c.add(ip, 20);
    put16(ip + 10, c.result());
    if (!ipBegin()) return false;
    ipWrite(ip, 20);
    ipWrite(hdr, hdrLen);
    if (n1 > 0) ipWrite(d1, n1);
    if (n2 > 0) ipWrite(d2, n2);
    return ipEnd();
  }

  void pseudoHeader(Checksum &c, uint8_t proto, ip4_t src, ip4_t dst,
                    size_t len) {
    c.add((const uint8_t *)&src, 4);
    c.add((const uint8_t *)&dst, 4);
    c.add16(proto);
    c.add16((uint16_t)len);
  }

  // ---- ICMP ----

  bool icmpInput(const uint8_t *data, size_t total, size_t ihl) {
    if (total < ihl + 8 || data[ihl] != 8) return false;  // echo request only
    if (!_ping) return false;
    const uint8_t *icmp = data + ihl;
    size_t len = total - ihl;
    Checksum c;
    c.add(icmp, len);
    if (c.result() != 0) return true;
    // echo reply: same content, type 0, new checksum
    uint8_t hdr[4] = {0, 0, 0, 0};
    Checksum r;
    r.add(hdr, 4);
    r.add(icmp + 4, len - 4);
    put16(hdr + 2, r.result());
    ip4_t src;
    memcpy(&src, data + 12, 4);
    sendIP(IP_ICMP, src, hdr, 4, icmp + 4, len - 4);
    return true;
  }

  // ---- UDP ----

  bool sendUdp(uint16_t srcPort, ip4_t dst, uint16_t dstPort,
               const uint8_t *data, size_t len) {
    if (!ipIsUp() || len + 28 > ipMtu()) return false;
    uint8_t hdr[8];
    put16(hdr, srcPort);
    put16(hdr + 2, dstPort);
    put16(hdr + 4, (uint16_t)(len + 8));
    put16(hdr + 6, 0);
    Checksum c;
    pseudoHeader(c, IP_UDP, ipLocal(), dst, len + 8);
    c.add(hdr, 8);
    c.add(data, len);
    uint16_t sum = c.result();
    put16(hdr + 6, sum == 0 ? 0xFFFF : sum);
    return sendIP(IP_UDP, dst, hdr, 8, data, len);
  }

  bool udpInput(ip4_t src, ip4_t dst, const uint8_t *p, size_t len) {
    if (len < 8) return false;
    size_t ulen = get16(p + 4);
    if (ulen < 8 || ulen > len) return false;
    if (get16(p + 6) != 0) {
      Checksum c;
      pseudoHeader(c, IP_UDP, src, dst, ulen);
      c.add(p, ulen);
      if (c.result() != 0) return true;  // corrupted: drop
    }
    uint16_t srcPort = get16(p);
    uint16_t dstPort = get16(p + 2);
    const uint8_t *data = p + 8;
    size_t dlen = ulen - 8;
    if (_dnsPort != 0 && dstPort == _dnsPort) {
      dnsInput(data, dlen);
      return true;
    }
    for (auto &u : _udp) {
      if (!u.inUse || u.localPort != dstPort) continue;
      if (u.rx.free() >= dlen + 8) {
        uint8_t hdr[8];
        memcpy(hdr, &src, 4);
        put16(hdr + 4, srcPort);
        put16(hdr + 6, (uint16_t)dlen);
        u.rx.write(hdr, 8);
        u.rx.write(data, dlen);
      }
      return true;
    }
    return false;
  }

  // ---- DNS ----

  bool dnsQuery(ip4_t server, const char *host, uint32_t timeoutMs,
                IPAddress &result) {
    uint8_t q[300];
    size_t n = 0;
    _dnsId = (uint16_t)random(0xFFFF);
    put16(q, _dnsId);
    put16(q + 2, 0x0100);  // recursion desired
    put16(q + 4, 1);       // one question
    memset(q + 6, 0, 6);
    n = 12;
    const char *label = host;
    while (*label) {
      const char *dot = strchr(label, '.');
      size_t l = dot ? (size_t)(dot - label) : strlen(label);
      if (l == 0 || l > 63 || n + l + 6 > sizeof(q)) return false;
      q[n++] = (uint8_t)l;
      memcpy(q + n, label, l);
      n += l;
      label += l;
      if (*label == '.') label++;
    }
    q[n++] = 0;
    put16(q + n, 1);  // type A
    put16(q + n + 2, 1);  // class IN
    n += 4;

    _dnsPort = ephemeralPort();
    _dnsDone = false;
    _dnsResult = 0;
    bool ok = false;
    uint32_t start = millis();
    uint32_t sent = 0;
    while (millis() - start < timeoutMs) {
      if (sent == 0 || millis() - sent >= 1000) {
        sendUdp(_dnsPort, server, 53, q, n);
        sent = millis();
        if (sent == 0) sent = 1;
      }
      idle();
      if (_dnsDone) {
        ok = _dnsResult != 0;
        break;
      }
    }
    _dnsPort = 0;
    if (ok) {
      const uint8_t *b = (const uint8_t *)&_dnsResult;
      result = IPAddress(b[0], b[1], b[2], b[3]);
    }
    return ok;
  }

  /// Skips an encoded (possibly compressed) name: returns the new offset or 0
  static size_t dnsSkipName(const uint8_t *p, size_t len, size_t pos) {
    while (pos < len) {
      uint8_t l = p[pos];
      if (l == 0) return pos + 1;
      if ((l & 0xC0) == 0xC0) return pos + 2;
      pos += l + 1;
    }
    return 0;
  }

  void dnsInput(const uint8_t *p, size_t len) {
    if (len < 12 || get16(p) != _dnsId || !(p[2] & 0x80)) return;
    _dnsDone = true;
    if ((p[3] & 0x0F) != 0) return;  // error
    size_t qd = get16(p + 4), an = get16(p + 6);
    size_t pos = 12;
    for (size_t i = 0; i < qd; i++) {
      pos = dnsSkipName(p, len, pos);
      if (pos == 0) return;
      pos += 4;
    }
    for (size_t i = 0; i < an; i++) {
      pos = dnsSkipName(p, len, pos);
      if (pos == 0 || pos + 10 > len) return;
      uint16_t type = get16(p + pos), cls = get16(p + pos + 2);
      size_t rdlen = get16(p + pos + 8);
      pos += 10;
      if (pos + rdlen > len) return;
      if (type == 1 && cls == 1 && rdlen == 4) {
        memcpy(&_dnsResult, p + pos, 4);
        return;
      }
      pos += rdlen;
    }
  }

  // ---- TCP ----

  int allocTcp() {
    int found = -1;
    for (int i = 0; i < TINYPPP_TCP_SOCKETS && found < 0; i++)
      if (!_tcp[i].inUse) found = i;
    // reuse a socket that is closing in the background
    for (int i = 0; i < TINYPPP_TCP_SOCKETS && found < 0; i++)
      if (_tcp[i].orphan && _tcp[i].state == TCPState::TimeWait) found = i;
    for (int i = 0; i < TINYPPP_TCP_SOCKETS && found < 0; i++) {
      if (_tcp[i].orphan) {
        sendReset(_tcp[i]);
        found = i;
      }
    }
    if (found < 0) return -1;
    TCPSocket &t = _tcp[found];
    uint8_t gen = t.gen + 1;
    t.inUse = true;
    t.orphan = t.accepted = t.ackPending = t.finQueued = t.finSent = false;
    t.gen = gen;
    t.retries = 0;
    t.state = TCPState::Closed;
    t.rx.clear();
    t.tx.clear();
    t.iss = (uint32_t)micros() * 2654435761u ^ (uint32_t)random(0x7FFFFFFF);
    t.sndUna = t.iss;
    t.sndNxt = t.iss + 1;
    t.rcvNxt = 0;
    t.sndWnd = 0;
    uint16_t mtu = ipMtu();
    t.mss = mtu > 40 ? mtu - 40 : 536;
    t.rto = TINYPPP_TCP_RTO;
    t.timer = t.deadline = 0;
    t.advWnd = 0;
    return found;
  }

  void freeTcp(TCPSocket &t) {
    t.inUse = false;
    t.orphan = false;
    t.state = TCPState::Closed;
    t.timer = t.deadline = 0;
    t.rx.clear();
    t.tx.clear();
  }

  static bool canSend(const TCPSocket &t) {
    return (t.state == TCPState::Established ||
            t.state == TCPState::CloseWait) &&
           !t.finQueued;
  }

  uint16_t window(TCPSocket &t) {
    size_t w = t.rx.free();
    if (w > 0xFFFF) w = 0xFFFF;
    t.advWnd = (uint16_t)w;
    return (uint16_t)w;
  }

  /// Our receive MSS
  uint16_t rcvMss() const {
    uint16_t mtu = ipMtu();
    uint16_t mss = mtu > 40 ? mtu - 40 : 536;
    if (mss > TINYPPP_TCP_RX_BUFFER) mss = TINYPPP_TCP_RX_BUFFER;
    return mss;
  }

  /// Sends a segment; data is taken from the tx buffer at offset
  bool sendSegment(TCPSocket &t, uint32_t seq, uint8_t flags, size_t offset,
                   size_t len) {
    const uint8_t *p1 = nullptr, *p2 = nullptr;
    size_t n1 = len > 0 ? t.tx.contiguous(offset, len, p1) : 0;
    size_t n2 = n1 < len ? t.tx.contiguous(offset + n1, len - n1, p2) : 0;
    if (n1 + n2 != len) return false;
    t.ackPending = false;
    return sendTcp(t.localPort, t.remotePort, t.remoteIP, seq, t.rcvNxt, flags,
                   window(t), p1, n1, p2, n2);
  }

  bool sendTcp(uint16_t localPort, uint16_t remotePort, ip4_t remoteIP,
               uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
               const uint8_t *p1 = nullptr, size_t n1 = 0,
               const uint8_t *p2 = nullptr, size_t n2 = 0) {
    uint8_t hdr[24];
    size_t hlen = (flags & TCP_SYN) ? 24 : 20;
    put16(hdr, localPort);
    put16(hdr + 2, remotePort);
    put32(hdr + 4, seq);
    put32(hdr + 8, (flags & TCP_ACK) ? ack : 0);
    hdr[12] = (uint8_t)((hlen / 4) << 4);
    hdr[13] = flags;
    put16(hdr + 14, wnd);
    put16(hdr + 16, 0);
    put16(hdr + 18, 0);
    if (hlen == 24) {
      hdr[20] = 2;  // MSS option
      hdr[21] = 4;
      put16(hdr + 22, rcvMss());
    }
    Checksum c;
    pseudoHeader(c, IP_TCP, ipLocal(), remoteIP, hlen + n1 + n2);
    c.add(hdr, hlen);
    if (n1) c.add(p1, n1);
    if (n2) c.add(p2, n2);
    put16(hdr + 16, c.result());
    return sendIP(IP_TCP, remoteIP, hdr, hlen, p1, n1, p2, n2);
  }

  void armTimer(TCPSocket &t) {
    t.timer = millis() + t.rto;
    if (t.timer == 0) t.timer = 1;
  }

  void sendSyn(TCPSocket &t) {
    uint8_t flags = TCP_SYN;
    if (t.state == TCPState::SynRcvd) flags |= TCP_ACK;
    sendSegment(t, t.iss, flags, 0, 0);
    armTimer(t);
  }

  void sendAck(TCPSocket &t) { sendSegment(t, t.sndNxt, TCP_ACK, 0, 0); }

  void sendReset(TCPSocket &t) {
    if (t.state == TCPState::Closed || t.state == TCPState::TimeWait) return;
    sendSegment(t, t.sndNxt, TCP_RST | TCP_ACK, 0, 0);
  }

  /// Answers a segment that has no connection with a reset
  void replyReset(ip4_t src, const uint8_t *p, size_t segLen) {
    uint8_t flags = p[13];
    if (flags & TCP_RST) return;
    if (flags & TCP_ACK) {
      sendTcp(get16(p + 2), get16(p), src, get32(p + 8), 0, TCP_RST, 0);
    } else {
      sendTcp(get16(p + 2), get16(p), src, 0, get32(p + 4) + segLen,
              TCP_RST | TCP_ACK, 0);
    }
  }

  /// Sends queued data (and FIN) as far as the peer window allows
  void output(TCPSocket &t) {
    if (t.state != TCPState::Established && t.state != TCPState::CloseWait &&
        t.state != TCPState::FinWait1 && t.state != TCPState::LastAck &&
        t.state != TCPState::Closing)
      return;
    size_t queued = t.tx.available();
    while (!t.finSent) {
      size_t offset = t.sndNxt - t.sndUna;
      if (offset >= queued) break;
      uint32_t inFlight = t.sndNxt - t.sndUna;
      if (inFlight >= t.sndWnd) break;
      size_t len = queued - offset;
      if (len > t.mss) len = t.mss;
      if (len > t.sndWnd - inFlight) len = t.sndWnd - inFlight;
      if (!sendSegment(t, t.sndNxt, TCP_ACK | TCP_PSH, offset, len)) break;
      t.sndNxt += len;
      if (t.timer == 0) armTimer(t);
    }
    if (t.finQueued && !t.finSent && t.sndNxt - t.sndUna == queued) {
      sendSegment(t, t.sndNxt, TCP_FIN | TCP_ACK, 0, 0);
      t.sndNxt++;
      t.finSent = true;
      if (t.state == TCPState::Established) t.state = TCPState::FinWait1;
      else if (t.state == TCPState::CloseWait) t.state = TCPState::LastAck;
      if (t.timer == 0) armTimer(t);
    }
    // zero window: the retransmission timer sends probes
    if (t.timer == 0 && queued > 0 && t.sndWnd == 0) armTimer(t);
  }

  /// Retransmits the oldest unacknowledged segment
  void retransmit(TCPSocket &t) {
    switch (t.state) {
      case TCPState::SynSent:
      case TCPState::SynRcvd:
        sendSyn(t);
        return;
      default:
        break;
    }
    size_t queued = t.tx.available();
    if (queued > 0) {
      size_t len = queued < t.mss ? queued : t.mss;
      // one byte probes into a zero window
      if (t.sndWnd == 0) len = 1;
      sendSegment(t, t.sndUna, TCP_ACK | TCP_PSH, 0, len);
      if (seqLT(t.sndNxt, t.sndUna + len)) t.sndNxt = t.sndUna + len;
    } else if (t.finSent) {
      sendSegment(t, t.sndUna, TCP_FIN | TCP_ACK, 0, 0);
    } else {
      t.timer = 0;
      return;
    }
    armTimer(t);
  }

  void windowUpdate(TCPSocket &t) {
    if (t.state != TCPState::Established && t.state != TCPState::FinWait1 &&
        t.state != TCPState::FinWait2)
      return;
    size_t free = t.rx.free();
    size_t threshold = t.rx.size() / 2;
    if (threshold > rcvMss()) threshold = rcvMss();
    if (t.advWnd < threshold && free >= threshold) sendAck(t);
  }

  void processTimers() {
    uint32_t now = millis();
    for (auto &t : _tcp) {
      if (!t.inUse) continue;
      if (t.deadline != 0 && (int32_t)(now - t.deadline) >= 0) {
        if (t.orphan) {
          if (t.state != TCPState::TimeWait) sendReset(t);
          freeTcp(t);
          continue;
        }
        t.deadline = 0;
        t.state = TCPState::Closed;
      }
      if (t.timer != 0 && (int32_t)(now - t.timer) >= 0) {
        if (++t.retries > TINYPPP_TCP_MAX_RETRIES) {
          sendReset(t);
          t.state = TCPState::Closed;
          t.timer = 0;
          if (t.orphan || !t.accepted) freeTcp(t);
          continue;
        }
        t.rto = t.rto * 2 > 16000 ? 16000 : t.rto * 2;
        retransmit(t);
      }
    }
  }

  void flushAcks() {
    for (auto &t : _tcp) {
      if (t.inUse && t.ackPending) sendAck(t);
    }
  }

  void enterTimeWait(TCPSocket &t) {
    t.state = TCPState::TimeWait;
    t.timer = 0;
    t.deadline = millis() + TINYPPP_TCP_TIME_WAIT;
    if (t.deadline == 0) t.deadline = 1;
  }

  void parseOptions(TCPSocket &t, const uint8_t *opt, size_t len) {
    size_t i = 0;
    while (i < len) {
      uint8_t kind = opt[i];
      if (kind == 0) break;
      if (kind == 1) {
        i++;
        continue;
      }
      if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) break;
      if (kind == 2 && opt[i + 1] == 4) {
        uint16_t mss = get16(opt + i + 2);
        if (mss >= 64 && mss < t.mss) t.mss = mss;
      }
      i += opt[i + 1];
    }
  }

  bool tcpInput(ip4_t src, ip4_t dst, const uint8_t *p, size_t len,
                bool reply) {
    if (len < 20) return false;
    size_t hlen = (p[12] >> 4) * 4;
    if (hlen < 20 || hlen > len) return false;
    Checksum c;
    pseudoHeader(c, IP_TCP, src, dst, len);
    c.add(p, len);
    if (c.result() != 0) return true;  // corrupted: drop

    uint16_t srcPort = get16(p), dstPort = get16(p + 2);
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    uint8_t flags = p[13];
    uint32_t wnd = get16(p + 14);
    const uint8_t *data = p + hlen;
    size_t dlen = len - hlen;
    size_t segLen = dlen + ((flags & TCP_SYN) ? 1 : 0) + ((flags & TCP_FIN) ? 1 : 0);

    TCPSocket *ts = nullptr;
    for (auto &t : _tcp) {
      if (t.inUse && t.state != TCPState::Closed && t.localPort == dstPort &&
          t.remotePort == srcPort && t.remoteIP == src) {
        ts = &t;
        break;
      }
    }

    if (ts == nullptr) {
      if ((flags & (TCP_SYN | TCP_ACK | TCP_RST)) == TCP_SYN &&
          listening(dstPort)) {
        int s = allocTcp();
        if (s < 0) return true;  // no socket: the peer will retry
        TCPSocket &t = _tcp[s];
        t.localPort = dstPort;
        t.remotePort = srcPort;
        t.remoteIP = src;
        t.rcvNxt = seq + 1;
        t.sndWnd = wnd;
        parseOptions(t, p + 20, hlen - 20);
        t.state = TCPState::SynRcvd;
        sendSyn(t);
        return true;
      }
      if (!reply) return false;
      replyReset(src, p, segLen);
      return true;
    }

    TCPSocket &t = *ts;

    if (t.state == TCPState::SynSent) {
      bool ackOk = (flags & TCP_ACK) && ack == t.iss + 1;
      if ((flags & TCP_ACK) && !ackOk) {
        replyReset(src, p, segLen);
        return true;
      }
      if (flags & TCP_RST) {
        if (ackOk) t.state = TCPState::Closed;  // connection refused
        return true;
      }
      if ((flags & TCP_SYN) && ackOk) {
        t.rcvNxt = seq + 1;
        t.sndUna = ack;
        t.sndWnd = wnd;
        parseOptions(t, p + 20, hlen - 20);
        t.state = TCPState::Established;
        t.timer = 0;
        t.retries = 0;
        t.rto = TINYPPP_TCP_RTO;
        sendAck(t);
      }
      return true;
    }

    // sequence number check: in-order segments only
    if (flags & TCP_RST) {
      if (seq == t.rcvNxt) {
        t.state = TCPState::Closed;
        t.timer = 0;
        if (t.orphan || !t.accepted) freeTcp(t);
      }
      return true;
    }
    if (flags & TCP_SYN) {
      // retransmitted SYN: our SYN-ACK was lost
      if (t.state == TCPState::SynRcvd && seq + 1 == t.rcvNxt) sendSyn(t);
      else t.ackPending = true;
      return true;
    }
    if (segLen > 0 && seq != t.rcvNxt) {
      if (seqLT(seq, t.rcvNxt) && seqGT(seq + dlen, t.rcvNxt)) {
        // partly old: skip what we already have
        size_t skip = t.rcvNxt - seq;
        data += skip;
        dlen -= skip;
        seq = t.rcvNxt;
      } else {
        t.ackPending = true;  // out of order or duplicate
        return true;
      }
    }
    if (!(flags & TCP_ACK)) return true;

    if (t.state == TCPState::SynRcvd) {
      if (ack != t.iss + 1) {
        replyReset(src, p, segLen);
        return true;
      }
      t.state = TCPState::Established;
      t.sndUna = ack;
      t.sndWnd = wnd;
      t.timer = 0;
      t.retries = 0;
      t.rto = TINYPPP_TCP_RTO;
    }

    // acknowledgment
    if (seqGT(ack, t.sndNxt)) {
      t.ackPending = true;
      return true;
    }
    t.sndWnd = wnd;
    bool finAcked = false;
    if (seqGT(ack, t.sndUna)) {
      size_t acked = ack - t.sndUna;
      size_t dataAcked = acked < t.tx.available() ? acked : t.tx.available();
      t.tx.discard(dataAcked);
      t.sndUna = ack;
      t.retries = 0;
      t.rto = TINYPPP_TCP_RTO;
      t.timer = 0;
      if (t.sndUna != t.sndNxt) armTimer(t);
    }
    if (t.finSent && t.sndUna == t.sndNxt) finAcked = true;

    switch (t.state) {
      case TCPState::FinWait1:
        if (finAcked) {
          t.state = TCPState::FinWait2;
          if (t.orphan) t.deadline = millis() + TINYPPP_TCP_LINGER;
        }
        break;
      case TCPState::Closing:
        if (finAcked) enterTimeWait(t);
        break;
      case TCPState::LastAck:
        if (finAcked) {
          t.state = TCPState::Closed;
          t.timer = 0;
          if (t.orphan) freeTcp(t);
          return true;
        }
        break;
      case TCPState::TimeWait:
        if (flags & TCP_FIN) {
          t.ackPending = true;
          enterTimeWait(t);
        }
        return true;
      default:
        break;
    }

    // data
    if (dlen > 0 && (t.state == TCPState::Established ||
                     t.state == TCPState::FinWait1 ||
                     t.state == TCPState::FinWait2)) {
      size_t n = t.orphan ? dlen : t.rx.write(data, dlen);
      t.rcvNxt += n;
      t.ackPending = true;
      if (n < dlen) flags &= ~TCP_FIN;  // FIN comes after the dropped data
    }

    if ((flags & TCP_FIN) && seq + dlen == t.rcvNxt) {
      t.rcvNxt++;
      t.ackPending = true;
      switch (t.state) {
        case TCPState::Established:
          t.state = TCPState::CloseWait;
          break;
        case TCPState::FinWait1:
          if (finAcked) enterTimeWait(t);
          else t.state = TCPState::Closing;
          break;
        case TCPState::FinWait2:
          enterTimeWait(t);
          break;
        default:
          break;
      }
    }

    output(t);
    return true;
  }
};

}  // namespace tinyppp
