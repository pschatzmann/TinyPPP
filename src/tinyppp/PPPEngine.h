#pragma once
/**
 * A small, portable PPP implementation (RFC 1661 LCP, RFC 1332 IPCP,
 * RFC 1334 PAP client, RFC 1877 DNS options) that runs over any Arduino
 * Stream. It does not contain an IP stack: received IPv4 packets are handed to
 * onIPv4Packet() and outgoing packets are passed to sendIPv4() or
 * beginIPv4()/writeIPv4()/endIPv4().
 *
 * The engine is not thread safe: the caller must serialize process() and the
 * send methods (e.g. by holding the lwIP lock).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>

#include "HDLC.h"

#ifndef TINYPPP_MRU
#define TINYPPP_MRU 1500
#endif

#ifndef TINYPPP_RESTART_MS
#define TINYPPP_RESTART_MS 2000
#endif

namespace tinyppp {

// PPP protocol numbers
static constexpr uint16_t PPP_IP = 0x0021;
static constexpr uint16_t PPP_IPV6 = 0x0057;
static constexpr uint16_t PPP_LCP = 0xC021;
static constexpr uint16_t PPP_PAP = 0xC023;
static constexpr uint16_t PPP_IPCP = 0x8021;

// Control protocol codes
enum CPCode : uint8_t {
  CONF_REQ = 1,
  CONF_ACK = 2,
  CONF_NAK = 3,
  CONF_REJ = 4,
  TERM_REQ = 5,
  TERM_ACK = 6,
  CODE_REJ = 7,
  PROT_REJ = 8,
  ECHO_REQ = 9,
  ECHO_REP = 10,
  DISCARD_REQ = 11,
};

// LCP options
enum LCPOption : uint8_t {
  LCP_MRU = 1,
  LCP_ACCM = 2,
  LCP_AUTH = 3,
  LCP_MAGIC = 5,
  LCP_PFC = 7,
  LCP_ACFC = 8,
};

// IPCP options
enum IPCPOption : uint8_t {
  IPCP_COMPRESSION = 2,
  IPCP_ADDRESS = 3,
  IPCP_DNS1 = 129,
  IPCP_DNS2 = 131,
};

/// Simplified RFC 1661 automaton states
enum class CPState : uint8_t {
  Initial,  // not started / closed by the user
  Stopped,  // waiting for the peer (or the restart timer)
  Closing,  // Terminate-Request sent
  ReqSent,
  AckRcvd,
  AckSent,
  Opened,
};

/// Overall link phase
enum class PPPPhase : uint8_t {
  Dead,
  Establish,
  Authenticate,
  Network,
  Running,
  Terminate,
};

inline const char *toStr(PPPPhase p) {
  switch (p) {
    case PPPPhase::Dead: return "Dead";
    case PPPPhase::Establish: return "Establish";
    case PPPPhase::Authenticate: return "Authenticate";
    case PPPPhase::Network: return "Network";
    case PPPPhase::Running: return "Running";
    case PPPPhase::Terminate: return "Terminate";
  }
  return "?";
}

/// IPv4 address in network byte order as stored in memory (same as lwIP)
using ip4_t = uint32_t;

inline ip4_t toIp4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  uint8_t v[4] = {a, b, c, d};
  ip4_t r;
  memcpy(&r, v, 4);
  return r;
}

/// Statistics
struct PPPStats {
  uint32_t rxFrames = 0;
  uint32_t txFrames = 0;
  uint32_t rxIP = 0;
  uint32_t txIP = 0;
  uint32_t rxDropped = 0;
  uint32_t txErrors = 0;
};

class PPPEngine {
 public:
  virtual ~PPPEngine() = default;

  /// Optional static address configuration. 0 = negotiate with the peer
  void setLocalIP(ip4_t ip) { _cfgLocal = ip; }
  /// Address to assign to the peer if it requests one (server role)
  void setPeerIP(ip4_t ip) { _cfgPeer = ip; }
  /// DNS servers to offer the peer if it asks for them (server role)
  void setOfferedDNS(ip4_t dns1, ip4_t dns2 = 0) {
    _cfgOfferDns1 = dns1;
    _cfgOfferDns2 = dns2;
  }
  /// Request DNS servers from the peer (default true)
  void setRequestDNS(bool flag) { _cfgRequestDns = flag; }
  /// PAP credentials (used only if the peer requires authentication)
  void setCredentials(const char *user, const char *password) {
    snprintf(_user, sizeof(_user), "%s", user ? user : "");
    snprintf(_password, sizeof(_password), "%s", password ? password : "");
  }
  /// Passive: do not send LCP requests before the peer starts negotiating
  void setPassive(bool passive) { _passive = passive; }
  /// LCP echo keep-alive: 0 disables it
  void setEchoInterval(uint32_t ms, uint8_t maxFailures = 3) {
    _echoInterval = ms;
    _echoMaxFail = maxFailures;
  }
  /// Debug output (nullptr to disable)
  void setLogger(Print *log) { _log = log; }

  /// Starts the PPP negotiation on the indicated stream
  void start(Stream &io) {
    _io = &io;
    _enc.setOutput(&io);
    _started = true;
    resetLink();
    _magic = newMagic();
    setPhase(PPPPhase::Establish);
    if (_passive) {
      _lcp.state = CPState::Stopped;
    } else {
      lcpOpen();
    }
  }

  /// Closes the link gracefully (sends LCP Terminate-Request)
  void stop() {
    if (!_started) return;
    if (_lcp.state == CPState::Opened || _lcp.state == CPState::AckSent ||
        _lcp.state == CPState::AckRcvd || _lcp.state == CPState::ReqSent) {
      static const uint8_t reason[] = "bye";
      sendCP(PPP_LCP, TERM_REQ, ++_lcp.id, reason, sizeof(reason) - 1);
    }
    ipcpDown();
    _lcp.state = CPState::Initial;
    _ipcp.state = CPState::Initial;
    _lcp.timer = _ipcp.timer = _papTimer = 0;
    _started = false;
    setPhase(PPPPhase::Dead);
  }

  /// Reads available input and runs the timers: call frequently
  void process() {
    if (!_started || _io == nullptr) return;
    uint8_t buf[128];
    // bound the work per call so that we do not block for too long
    for (int loops = 0; loops < 32; loops++) {
      int avail = _io->available();
      if (avail <= 0) break;
      size_t n = _io->readBytes(
          buf, (size_t)avail < sizeof(buf) ? (size_t)avail : sizeof(buf));
      if (n == 0) break;
      for (size_t i = 0; i < n; i++) {
        if (_dec.add(buf[i])) handleFrame(_dec.frame(), _dec.frameLen());
      }
    }
    processTimers(millis());
  }

  /// Feeds raw bytes (alternative to process() when you read the stream
  /// yourself)
  void input(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
      if (_dec.add(data[i])) handleFrame(_dec.frame(), _dec.frameLen());
    }
  }

  /// Runs the retransmission and keep-alive timers
  void processTimers(uint32_t now) {
    if (!_started) return;
    if (_lcp.timer && (int32_t)(now - _lcp.timer) >= 0) {
      _lcp.timer = 0;
      lcpTimeout();
    }
    if (_ipcp.timer && (int32_t)(now - _ipcp.timer) >= 0) {
      _ipcp.timer = 0;
      ipcpTimeout();
    }
    if (_papTimer && (int32_t)(now - _papTimer) >= 0) {
      _papTimer = 0;
      papTimeout();
    }
    if (_echoInterval && _lcp.state == CPState::Opened &&
        (int32_t)(now - _echoNext) >= 0) {
      _echoNext = now + _echoInterval;
      if (_echoPending >= _echoMaxFail) {
        logf("LCP echo timeout: restarting link");
        lcpDown();
        lcpOpen();
        return;
      }
      _echoPending++;
      uint8_t data[4];
      memcpy(data, &_magic, 4);
      sendCP(PPP_LCP, ECHO_REQ, ++_echoId, data, 4);
    }
  }

  /// Sends a complete IPv4 packet
  bool sendIPv4(const uint8_t *data, size_t len) {
    if (!beginIPv4()) return false;
    writeIPv4(data, len);
    return endIPv4();
  }

  /// Streaming API for chained buffers (e.g. lwIP pbuf)
  bool beginIPv4() {
    if (!isUp()) return false;
    _enc.begin(PPP_IP);
    return true;
  }
  void writeIPv4(const uint8_t *data, size_t len) { _enc.data(data, len); }
  bool endIPv4() {
    bool ok = _enc.end();
    _stats.txFrames++;
    if (ok) {
      _stats.txIP++;
    } else {
      _stats.txErrors++;
    }
    return ok;
  }

  /// true when IPCP is open and IPv4 packets can be exchanged
  bool isUp() const { return _ipcp.state == CPState::Opened; }
  bool isStarted() const { return _started; }
  PPPPhase phase() const { return _phase; }
  ip4_t localIP() const { return _ourIP; }
  ip4_t peerIP() const { return _peerIP; }
  ip4_t dns1() const { return _dns1; }
  ip4_t dns2() const { return _dns2; }
  /// MTU we may use for sending (peer MRU)
  uint16_t mtu() const { return _peerMRU; }
  const PPPStats &stats() const { return _stats; }
  uint32_t fcsErrors() const { return _dec.fcsErrors(); }

 protected:
  /// Called for each received IPv4 packet
  virtual void onIPv4Packet(const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
  }
  /// Called when IPCP is opened: addresses are available
  virtual void onLinkUp() {}
  /// Called when the IP link goes down
  virtual void onLinkDown() {}
  /// Called on phase changes
  virtual void onPhase(PPPPhase phase) { (void)phase; }

  void logf(const char *fmt, ...) {
    if (_log == nullptr) return;
    char msg[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    _log->print("PPP: ");
    _log->println(msg);
  }

 private:
  struct ControlProtocol {
    uint16_t proto;
    CPState state = CPState::Initial;
    uint8_t id = 0;       // id of the last request we sent
    uint8_t retries = 0;  // timeouts in a row
    uint32_t timer = 0;   // deadline in ms, 0 = stopped
  };

  Stream *_io = nullptr;
  Print *_log = nullptr;
  HDLCEncoder _enc;
  HDLCDecoder<TINYPPP_MRU + 4> _dec;
  PPPStats _stats;
  PPPPhase _phase = PPPPhase::Dead;
  bool _started = false;
  bool _passive = false;

  ControlProtocol _lcp{PPP_LCP};
  ControlProtocol _ipcp{PPP_IPCP};

  // LCP: our options
  bool _reqAccm = true;
  bool _reqMagic = true;
  uint32_t _magic = 0;
  // LCP: peer options
  uint16_t _peerMRU = 1500;
  uint32_t _peerAccm = 0xFFFFFFFF;
  bool _needPap = false;

  // PAP
  char _user[64] = "";
  char _password[64] = "";
  uint8_t _papId = 0;
  uint8_t _papRetries = 0;
  uint32_t _papTimer = 0;

  // echo
  uint32_t _echoInterval = 0;
  uint8_t _echoMaxFail = 3;
  uint8_t _echoPending = 0;
  uint8_t _echoId = 0;
  uint32_t _echoNext = 0;

  // IPCP configuration
  ip4_t _cfgLocal = 0;
  ip4_t _cfgPeer = 0;
  ip4_t _cfgOfferDns1 = 0;
  ip4_t _cfgOfferDns2 = 0;
  bool _cfgRequestDns = true;
  // IPCP negotiated values
  ip4_t _ourIP = 0;
  ip4_t _peerIP = 0;
  ip4_t _dns1 = 0;
  ip4_t _dns2 = 0;
  bool _reqAddr = true;
  bool _reqDns1 = true;
  bool _reqDns2 = true;

  // ---------------------------------------------------------------- helpers

  static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8) | p[1]; }
  static void put16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v & 0xFF;
  }

  uint32_t newMagic() {
    uint32_t m = (uint32_t)micros() * 2654435761u ^ (uint32_t)random(0x7FFFFFFF) ^ _magic;
    return m == 0 ? 0x5A5A1234 : m;
  }

  void setPhase(PPPPhase p) {
    if (p == _phase) return;
    _phase = p;
    logf("phase %s", toStr(p));
    onPhase(p);
  }

  void armTimer(ControlProtocol &cp, uint32_t ms = TINYPPP_RESTART_MS) {
    uint32_t t = millis() + ms;
    cp.timer = t == 0 ? 1 : t;
  }

  void resetLink() {
    _lcp.state = _ipcp.state = CPState::Initial;
    _lcp.timer = _ipcp.timer = _papTimer = 0;
    _reqAccm = _reqMagic = true;
    _peerMRU = 1500;
    _peerAccm = 0xFFFFFFFF;
    _enc.setACCM(_peerAccm);
    _needPap = false;
    _echoPending = 0;
    ipcpReset();
  }

  void ipcpReset() {
    _ourIP = _cfgLocal;
    _peerIP = 0;
    _dns1 = _dns2 = 0;
    _reqAddr = true;
    _reqDns1 = _reqDns2 = _cfgRequestDns;
  }

  void sendFrame(uint16_t proto, const uint8_t *data, size_t len) {
    // LCP frames always use the default ACCM (RFC 1662 7.1)
    _enc.begin(proto, proto == PPP_LCP);
    _enc.data(data, len);
    if (!_enc.end()) _stats.txErrors++;
    _stats.txFrames++;
  }

  void sendCP(uint16_t proto, uint8_t code, uint8_t id, const uint8_t *data,
              size_t len) {
    uint8_t pkt[4 + 128];
    if (len > sizeof(pkt) - 4) len = sizeof(pkt) - 4;
    pkt[0] = code;
    pkt[1] = id;
    put16(pkt + 2, (uint16_t)(len + 4));
    if (len) memcpy(pkt + 4, data, len);
    sendFrame(proto, pkt, len + 4);
  }

  // ------------------------------------------------------------- receiving

  void handleFrame(uint8_t *frame, size_t len) {
    _stats.rxFrames++;
    // address and control field may be compressed (ACFC)
    if (len >= 2 && frame[0] == HDLC_ALL_STATIONS && frame[1] == HDLC_UI) {
      frame += 2;
      len -= 2;
    }
    if (len < 1) return;
    // protocol field may be compressed (PFC): odd first byte
    uint16_t proto;
    if (frame[0] & 0x01) {
      proto = frame[0];
      frame += 1;
      len -= 1;
    } else {
      if (len < 2) return;
      proto = get16(frame);
      frame += 2;
      len -= 2;
    }

    switch (proto) {
      case PPP_IP:
        if (isUp()) {
          _stats.rxIP++;
          onIPv4Packet(frame, len);
        } else {
          _stats.rxDropped++;
        }
        return;
      case PPP_LCP:
        handleCP(_lcp, frame, len);
        return;
      case PPP_IPCP:
        if (_lcp.state == CPState::Opened && _phase >= PPPPhase::Network) {
          handleCP(_ipcp, frame, len);
        } else {
          _stats.rxDropped++;
        }
        return;
      case PPP_PAP:
        if (_lcp.state == CPState::Opened) handlePap(frame, len);
        return;
      default:
        _stats.rxDropped++;
        // unsupported protocol (CCP, IPV6CP, ...)
        if (_lcp.state == CPState::Opened) sendProtocolReject(proto, frame, len);
        return;
    }
  }

  void sendProtocolReject(uint16_t proto, const uint8_t *info, size_t len) {
    logf("protocol reject 0x%04x", proto);
    uint8_t data[2 + 64];
    put16(data, proto);
    if (len > sizeof(data) - 2) len = sizeof(data) - 2;
    memcpy(data + 2, info, len);
    sendCP(PPP_LCP, PROT_REJ, ++_lcp.id, data, len + 2);
  }

  void handleCP(ControlProtocol &cp, const uint8_t *pkt, size_t len) {
    if (len < 4) return;
    uint8_t code = pkt[0];
    uint8_t id = pkt[1];
    size_t plen = get16(pkt + 2);
    if (plen < 4 || plen > len) return;  // malformed
    const uint8_t *data = pkt + 4;
    size_t dlen = plen - 4;
    bool isLcp = cp.proto == PPP_LCP;

    if (cp.state == CPState::Initial) {
      // not started (e.g. IPCP before LCP is up or after stop())
      return;
    }

    switch (code) {
      case CONF_REQ:
        receiveConfigRequest(cp, id, data, dlen);
        break;
      case CONF_ACK:
        if (id != cp.id) return;
        receiveConfigAck(cp);
        break;
      case CONF_NAK:
      case CONF_REJ:
        if (id != cp.id) return;
        if (isLcp) {
          lcpApplyNakRej(code, data, dlen);
        } else {
          ipcpApplyNakRej(code, data, dlen);
        }
        receiveConfigNak(cp);
        break;
      case TERM_REQ:
        logf("%s terminate request", isLcp ? "LCP" : "IPCP");
        sendCP(cp.proto, TERM_ACK, id, nullptr, 0);
        if (isLcp) {
          lcpDown();
          // wait for the peer to come back
          _lcp.state = CPState::Stopped;
          if (!_passive) armTimer(_lcp, TINYPPP_RESTART_MS);
        } else {
          if (cp.state == CPState::Opened) ipcpDown();
          cp.state = CPState::Stopped;
          armTimer(cp, TINYPPP_RESTART_MS);
        }
        break;
      case TERM_ACK:
        if (cp.state == CPState::Opened) {
          if (isLcp) {
            lcpDown();
            lcpOpen();
          } else {
            ipcpDown();
            ipcpOpen();
          }
        } else if (cp.state == CPState::Closing) {
          cp.state = CPState::Stopped;
          if (isLcp && !_passive) armTimer(cp, TINYPPP_RESTART_MS);
        }
        break;
      case CODE_REJ:
        logf("code reject");
        break;
      case PROT_REJ:
        if (isLcp && dlen >= 2) {
          uint16_t p = get16(data);
          logf("peer rejected protocol 0x%04x", p);
          if (p == PPP_IPCP) {
            ipcpDown();
            _ipcp.state = CPState::Initial;
            _ipcp.timer = 0;
          }
        }
        break;
      case ECHO_REQ:
        if (isLcp && cp.state == CPState::Opened) {
          uint8_t reply[64];
          size_t n = dlen < sizeof(reply) ? dlen : sizeof(reply);
          memcpy(reply, data, n);
          if (n >= 4) memcpy(reply, &_magic, 4);
          sendCP(PPP_LCP, ECHO_REP, id, reply, n);
        }
        break;
      case ECHO_REP:
        _echoPending = 0;
        break;
      case DISCARD_REQ:
        break;
      default: {
        // unknown code: Code-Reject
        sendCP(cp.proto, CODE_REJ, ++cp.id, pkt, plen);
        break;
      }
    }
  }

  // RFC 1661 automaton (simplified)

  void receiveConfigRequest(ControlProtocol &cp, uint8_t id,
                            const uint8_t *data, size_t len) {
    uint8_t reply[128];
    size_t replyLen = 0;
    uint8_t code = cp.proto == PPP_LCP
                       ? lcpCheckRequest(data, len, reply, replyLen)
                       : ipcpCheckRequest(data, len, reply, replyLen);
    if (code == 0) return;  // malformed
    bool ack = code == CONF_ACK;

    switch (cp.state) {
      case CPState::Opened:
        // renegotiation
        layerDown(cp);
        sendConfigRequest(cp);
        cp.state = ack ? CPState::AckSent : CPState::ReqSent;
        break;
      case CPState::Stopped:
      case CPState::Closing:
        sendConfigRequest(cp);
        cp.state = ack ? CPState::AckSent : CPState::ReqSent;
        break;
      case CPState::ReqSent:
      case CPState::AckSent:
        cp.state = ack ? CPState::AckSent : CPState::ReqSent;
        break;
      case CPState::AckRcvd:
        if (ack) {
          sendCP(cp.proto, code, id, reply, replyLen);
          cp.state = CPState::Opened;
          layerUp(cp);
          return;
        }
        break;
      default:
        break;
    }
    sendCP(cp.proto, code, id, reply, replyLen);
  }

  void receiveConfigAck(ControlProtocol &cp) {
    cp.retries = 0;
    switch (cp.state) {
      case CPState::ReqSent:
        cp.state = CPState::AckRcvd;
        armTimer(cp);
        break;
      case CPState::AckSent:
        cp.state = CPState::Opened;
        cp.timer = 0;
        layerUp(cp);
        break;
      case CPState::AckRcvd:
        // crossed connection
        sendConfigRequest(cp);
        cp.state = CPState::ReqSent;
        break;
      case CPState::Opened:
        layerDown(cp);
        sendConfigRequest(cp);
        cp.state = CPState::ReqSent;
        break;
      default:
        break;
    }
  }

  void receiveConfigNak(ControlProtocol &cp) {
    switch (cp.state) {
      case CPState::ReqSent:
      case CPState::AckSent:
        sendConfigRequest(cp);
        break;
      case CPState::AckRcvd:
        sendConfigRequest(cp);
        cp.state = CPState::ReqSent;
        break;
      case CPState::Opened:
        layerDown(cp);
        sendConfigRequest(cp);
        cp.state = CPState::ReqSent;
        break;
      default:
        break;
    }
  }

  void sendConfigRequest(ControlProtocol &cp) {
    uint8_t opts[32];
    size_t len = cp.proto == PPP_LCP ? lcpBuildRequest(opts) : ipcpBuildRequest(opts);
    sendCP(cp.proto, CONF_REQ, ++cp.id, opts, len);
    armTimer(cp);
  }

  void layerUp(ControlProtocol &cp) {
    if (cp.proto == PPP_LCP) {
      lcpUp();
    } else {
      ipcpUp();
    }
  }

  void layerDown(ControlProtocol &cp) {
    if (cp.proto == PPP_LCP) {
      lcpDown();
    } else {
      ipcpDown();
    }
  }

  // ------------------------------------------------------------------- LCP

  void lcpOpen() {
    _lcp.retries = 0;
    _reqAccm = _reqMagic = true;
    setPhase(PPPPhase::Establish);
    sendConfigRequest(_lcp);
    _lcp.state = CPState::ReqSent;
  }

  void lcpTimeout() {
    switch (_lcp.state) {
      case CPState::ReqSent:
      case CPState::AckRcvd:
      case CPState::AckSent:
        // keep on trying forever: the host might start pppd later
        if (++_lcp.retries > 10) _lcp.retries = 10;
        sendConfigRequest(_lcp);
        if (_lcp.state == CPState::AckRcvd) _lcp.state = CPState::ReqSent;
        // back off to reduce noise on the line while nobody is listening
        if (_lcp.retries >= 10) armTimer(_lcp, 5 * TINYPPP_RESTART_MS);
        break;
      case CPState::Stopped:
      case CPState::Closing:
        if (!_passive) lcpOpen();
        break;
      default:
        break;
    }
  }

  void lcpUp() {
    logf("LCP up (peer MRU %u)", _peerMRU);
    _enc.setACCM(_peerAccm);
    _echoPending = 0;
    _echoNext = millis() + _echoInterval;
    if (_needPap) {
      setPhase(PPPPhase::Authenticate);
      _papRetries = 0;
      papSend();
    } else {
      setPhase(PPPPhase::Network);
      ipcpOpen();
    }
  }

  void lcpDown() {
    if (_ipcp.state == CPState::Opened) ipcpDown();
    _ipcp.state = CPState::Initial;
    _ipcp.timer = 0;
    _papTimer = 0;
    _peerAccm = 0xFFFFFFFF;
    _enc.setACCM(_peerAccm);
    _needPap = false;
    if (_lcp.state == CPState::Opened) logf("LCP down");
    setPhase(_started ? PPPPhase::Establish : PPPPhase::Dead);
  }

  size_t lcpBuildRequest(uint8_t *p) {
    size_t n = 0;
    if (_reqAccm) {
      p[n++] = LCP_ACCM;
      p[n++] = 6;
      memset(p + n, 0, 4);  // nothing needs to be escaped
      n += 4;
    }
    if (_reqMagic) {
      p[n++] = LCP_MAGIC;
      p[n++] = 6;
      memcpy(p + n, &_magic, 4);
      n += 4;
    }
    return n;
  }

  void lcpApplyNakRej(uint8_t code, const uint8_t *p, size_t len) {
    size_t pos = 0;
    while (pos + 2 <= len) {
      uint8_t type = p[pos];
      uint8_t olen = p[pos + 1];
      if (olen < 2 || pos + olen > len) return;
      if (type == LCP_ACCM) {
        _reqAccm = false;  // keep the default map
      } else if (type == LCP_MAGIC) {
        if (code == CONF_REJ) {
          _reqMagic = false;
        } else {
          _magic = newMagic();
        }
      }
      pos += olen;
    }
  }

  uint8_t lcpCheckRequest(const uint8_t *p, size_t len, uint8_t *reply,
                          size_t &replyLen) {
    uint8_t naks[32];
    size_t nakLen = 0;
    size_t rejLen = 0;
    uint16_t mru = 1500;
    uint32_t accm = 0xFFFFFFFF;
    bool needPap = false;

    size_t pos = 0;
    while (pos < len) {
      if (pos + 2 > len) return 0;
      uint8_t type = p[pos];
      uint8_t olen = p[pos + 1];
      if (olen < 2 || pos + olen > len) return 0;
      const uint8_t *opt = p + pos;
      bool reject = false;
      switch (type) {
        case LCP_MRU:
          if (olen != 4) {
            reject = true;
          } else {
            mru = get16(opt + 2);
            if (mru > TINYPPP_MRU) mru = TINYPPP_MRU;
          }
          break;
        case LCP_ACCM:
          if (olen != 6) {
            reject = true;
          } else {
            accm = (uint32_t)opt[2] << 24 | (uint32_t)opt[3] << 16 |
                   (uint32_t)opt[4] << 8 | opt[5];
          }
          break;
        case LCP_AUTH:
          if (olen >= 4 && get16(opt + 2) == PPP_PAP) {
            needPap = true;
          } else if (nakLen + 4 <= sizeof(naks)) {
            // we only support PAP
            naks[nakLen++] = LCP_AUTH;
            naks[nakLen++] = 4;
            put16(naks + nakLen, PPP_PAP);
            nakLen += 2;
          }
          break;
        case LCP_MAGIC:
          if (olen != 6) {
            reject = true;
          } else if (_reqMagic && memcmp(opt + 2, &_magic, 4) == 0 &&
                     nakLen + 6 <= sizeof(naks)) {
            // looped back line or same magic: suggest another value
            uint32_t m = newMagic();
            naks[nakLen++] = LCP_MAGIC;
            naks[nakLen++] = 6;
            memcpy(naks + nakLen, &m, 4);
            nakLen += 4;
          }
          break;
        case LCP_PFC:
        case LCP_ACFC:
          // we accept compressed frames but never send them
          if (olen != 2) reject = true;
          break;
        default:
          reject = true;
          break;
      }
      if (reject && rejLen + olen <= 128) {
        memcpy(reply + rejLen, opt, olen);
        rejLen += olen;
      }
      pos += olen;
    }

    if (rejLen > 0) {
      replyLen = rejLen;
      return CONF_REJ;
    }
    if (nakLen > 0) {
      memcpy(reply, naks, nakLen);
      replyLen = nakLen;
      return CONF_NAK;
    }
    // Ack: commit the peer's options
    _peerMRU = mru;
    _peerAccm = accm;
    _needPap = needPap;
    size_t n = len < 128 ? len : 128;
    memcpy(reply, p, n);
    replyLen = n;
    return CONF_ACK;
  }

  // ------------------------------------------------------------------- PAP

  void papSend() {
    uint8_t data[2 + sizeof(_user) + sizeof(_password)];
    size_t ul = strlen(_user);
    size_t pl = strlen(_password);
    size_t n = 0;
    data[n++] = (uint8_t)ul;
    memcpy(data + n, _user, ul);
    n += ul;
    data[n++] = (uint8_t)pl;
    memcpy(data + n, _password, pl);
    n += pl;
    uint8_t pkt[4 + sizeof(data)];
    pkt[0] = 1;  // Authenticate-Request
    pkt[1] = ++_papId;
    put16(pkt + 2, (uint16_t)(n + 4));
    memcpy(pkt + 4, data, n);
    sendFrame(PPP_PAP, pkt, n + 4);
    _papTimer = millis() + TINYPPP_RESTART_MS;
    if (_papTimer == 0) _papTimer = 1;
  }

  void papTimeout() {
    if (_phase != PPPPhase::Authenticate) return;
    if (++_papRetries > 10) {
      logf("PAP: no response");
      lcpClose();
      return;
    }
    papSend();
  }

  void handlePap(const uint8_t *pkt, size_t len) {
    if (len < 4 || _phase != PPPPhase::Authenticate) return;
    uint8_t code = pkt[0];
    if (pkt[1] != _papId) return;
    if (code == 2) {  // Authenticate-Ack
      logf("PAP: authenticated");
      _papTimer = 0;
      setPhase(PPPPhase::Network);
      ipcpOpen();
    } else if (code == 3) {  // Authenticate-Nak
      logf("PAP: authentication failed");
      _papTimer = 0;
      lcpClose();
    }
  }

  void lcpClose() {
    static const uint8_t reason[] = "close";
    lcpDown();
    sendCP(PPP_LCP, TERM_REQ, ++_lcp.id, reason, sizeof(reason) - 1);
    _lcp.state = CPState::Closing;
    setPhase(PPPPhase::Terminate);
    armTimer(_lcp, 3 * TINYPPP_RESTART_MS);
  }

  // ------------------------------------------------------------------ IPCP

  void ipcpOpen() {
    ipcpReset();
    _ipcp.retries = 0;
    sendConfigRequest(_ipcp);
    _ipcp.state = CPState::ReqSent;
  }

  void ipcpTimeout() {
    switch (_ipcp.state) {
      case CPState::ReqSent:
      case CPState::AckRcvd:
      case CPState::AckSent:
        if (++_ipcp.retries > 10) {
          logf("IPCP: no response, restarting LCP");
          lcpClose();
          return;
        }
        sendConfigRequest(_ipcp);
        if (_ipcp.state == CPState::AckRcvd) _ipcp.state = CPState::ReqSent;
        break;
      case CPState::Stopped:
        if (_lcp.state == CPState::Opened) ipcpOpen();
        break;
      default:
        break;
    }
  }

  void ipcpUp() {
    if (_ourIP == 0) {
      logf("IPCP: no local address");
      lcpClose();
      return;
    }
    setPhase(PPPPhase::Running);
    char a[16], b[16];
    logf("IPCP up: local %s peer %s", ipStr(_ourIP, a), ipStr(_peerIP, b));
    onLinkUp();
  }

  void ipcpDown() {
    if (_ipcp.state != CPState::Opened) return;
    logf("IPCP down");
    _ipcp.state = CPState::Stopped;
    onLinkDown();
    if (_phase == PPPPhase::Running) setPhase(PPPPhase::Network);
  }

  static void putAddr(uint8_t *p, size_t &n, uint8_t type, ip4_t addr) {
    p[n++] = type;
    p[n++] = 6;
    memcpy(p + n, &addr, 4);
    n += 4;
  }

  size_t ipcpBuildRequest(uint8_t *p) {
    size_t n = 0;
    if (_reqAddr) putAddr(p, n, IPCP_ADDRESS, _ourIP);
    if (_reqDns1) putAddr(p, n, IPCP_DNS1, _dns1);
    if (_reqDns2) putAddr(p, n, IPCP_DNS2, _dns2);
    return n;
  }

  void ipcpApplyNakRej(uint8_t code, const uint8_t *p, size_t len) {
    size_t pos = 0;
    while (pos + 2 <= len) {
      uint8_t type = p[pos];
      uint8_t olen = p[pos + 1];
      if (olen < 2 || pos + olen > len) return;
      bool rej = code == CONF_REJ;
      ip4_t v = 0;
      if (olen == 6) memcpy(&v, p + pos + 2, 4);
      switch (type) {
        case IPCP_ADDRESS:
          if (rej) {
            _reqAddr = false;
          } else {
            _ourIP = v;
          }
          break;
        case IPCP_DNS1:
          if (rej) {
            _reqDns1 = false;
          } else {
            _dns1 = v;
          }
          break;
        case IPCP_DNS2:
          if (rej) {
            _reqDns2 = false;
          } else {
            _dns2 = v;
          }
          break;
        default:
          break;
      }
      pos += olen;
    }
  }

  uint8_t ipcpCheckRequest(const uint8_t *p, size_t len, uint8_t *reply,
                           size_t &replyLen) {
    uint8_t naks[32];
    size_t nakLen = 0;
    size_t rejLen = 0;
    ip4_t peer = 0;

    size_t pos = 0;
    while (pos < len) {
      if (pos + 2 > len) return 0;
      uint8_t type = p[pos];
      uint8_t olen = p[pos + 1];
      if (olen < 2 || pos + olen > len) return 0;
      const uint8_t *opt = p + pos;
      ip4_t v = 0;
      if (olen == 6) memcpy(&v, opt + 2, 4);
      bool reject = false;
      switch (type) {
        case IPCP_ADDRESS:
          if (olen != 6) {
            reject = true;
          } else if (v == 0 || (v == _ourIP && _ourIP != 0)) {
            // the peer wants an address from us
            if (_cfgPeer != 0 && _cfgPeer != _ourIP) {
              putAddr(naks, nakLen, IPCP_ADDRESS, _cfgPeer);
            } else {
              reject = true;
            }
          } else {
            peer = v;
          }
          break;
        case IPCP_DNS1:
        case IPCP_DNS2: {
          ip4_t offer = type == IPCP_DNS1 ? _cfgOfferDns1 : _cfgOfferDns2;
          if (olen != 6 || offer == 0) {
            reject = true;
          } else if (v != offer) {
            putAddr(naks, nakLen, type, offer);
          }
          break;
        }
        default:
          // IP compression (VJ) etc. are not supported
          reject = true;
          break;
      }
      if (reject && rejLen + olen <= 128) {
        memcpy(reply + rejLen, opt, olen);
        rejLen += olen;
      }
      pos += olen;
    }

    if (rejLen > 0) {
      replyLen = rejLen;
      return CONF_REJ;
    }
    if (nakLen > 0) {
      memcpy(reply, naks, nakLen);
      replyLen = nakLen;
      return CONF_NAK;
    }
    _peerIP = peer != 0 ? peer : _cfgPeer;
    size_t n = len < 128 ? len : 128;
    memcpy(reply, p, n);
    replyLen = n;
    return CONF_ACK;
  }

 protected:
  static const char *ipStr(ip4_t ip, char *buf) {
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
  }
};

/// Converts an engine address to an Arduino IPAddress
inline IPAddress toIPAddress(ip4_t ip) {
  const uint8_t *b = (const uint8_t *)&ip;
  return IPAddress(b[0], b[1], b[2], b[3]);
}

/// Converts an Arduino IPAddress to an engine address (0 if not set)
inline ip4_t fromIPAddress(const IPAddress &ip) {
  return toIp4(ip[0], ip[1], ip[2], ip[3]);
}

}  // namespace tinyppp
