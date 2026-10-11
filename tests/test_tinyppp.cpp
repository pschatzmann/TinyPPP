// Desktop unit tests for TinyPPP (run with ctest)
#include <stdio.h>

#include <vector>

#include "LoopbackStream.h"
#include "TinyPPP.h"

using namespace tinyppp;

static int failures = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      failures++;                                                   \
    }                                                               \
  } while (0)

/// Runs both sides until the condition is true or the timeout expires
template <class F>
static bool runUntil(F cond, std::vector<TinyPPP *> ppps, uint32_t ms = 5000) {
  uint32_t start = millis();
  while (!cond()) {
    if (millis() - start > ms) return false;
    for (auto p : ppps) p->loop();
    delay(1);
  }
  return true;
}

static void testFcs() {
  printf("testFcs\n");
  const char *check = "123456789";
  uint16_t fcs = ~fcs16((const uint8_t *)check, 9);
  CHECK(fcs == 0x906E);  // CRC-16/X-25 check value
}

static void testHdlc() {
  printf("testHdlc\n");
  LoopbackStream a, b;
  a.connect(b);
  HDLCEncoder enc;
  enc.setOutput(&a);
  enc.setACCM(0);
  uint8_t data[] = {0x7E, 0x7D, 0x00, 0x11, 0x20, 0xFF, 0x13};
  enc.begin(PPP_IP);
  enc.data(data, sizeof(data));
  CHECK(enc.end());

  HDLCDecoder<1504> dec;
  int frames = 0;
  while (b.available()) {
    if (dec.add(b.read())) {
      frames++;
      CHECK(dec.frameLen() == 4 + sizeof(data));
      CHECK(dec.frame()[0] == 0xFF && dec.frame()[1] == 0x03);
      CHECK(dec.frame()[2] == 0x00 && dec.frame()[3] == 0x21);
      CHECK(memcmp(dec.frame() + 4, data, sizeof(data)) == 0);
    }
  }
  CHECK(frames == 1);
  CHECK(dec.fcsErrors() == 0);

  // corrupted frame must be dropped
  enc.begin(PPP_IP);
  enc.data(data, sizeof(data));
  enc.end();
  std::vector<uint8_t> raw;
  while (b.available()) raw.push_back(b.read());
  raw[6] ^= 0x01;
  frames = 0;
  for (auto v : raw) frames += dec.add(v);
  CHECK(frames == 0);
  CHECK(dec.fcsErrors() == 1);
}

/// Builds an ICMP echo request
static std::vector<uint8_t> icmpEcho(ip4_t src, ip4_t dst, uint16_t seq) {
  std::vector<uint8_t> p(28 + 8, 0);
  p[0] = 0x45;
  p[3] = (uint8_t)p.size();
  p[8] = 64;
  p[9] = 1;
  memcpy(&p[12], &src, 4);
  memcpy(&p[16], &dst, 4);
  p[20] = 8;  // echo request
  p[26] = seq >> 8;
  p[27] = seq & 0xFF;
  Checksum ip, icmp;
  ip.add(p.data(), 20);
  p[10] = ip.result() >> 8;
  p[11] = ip.result() & 0xFF;
  icmp.add(p.data() + 20, p.size() - 20);
  p[22] = icmp.result() >> 8;
  p[23] = icmp.result() & 0xFF;
  return p;
}

static void testTwoEngines() {
  printf("testTwoEngines\n");
  LoopbackStream sa, sb;
  sa.connect(sb);
  TinyPPP server, client;
  server.config(IPAddress(10, 0, 0, 1), IPAddress(10, 0, 0, 2),
                IPAddress(8, 8, 8, 8), IPAddress(1, 1, 1, 1));
  server.setRequestDNS(false);
  server.begin(sa);
  client.begin(sb);

  CHECK(runUntil([&] { return server.connected() && client.connected(); },
                 {&server, &client}));
  CHECK(server.localIP() == IPAddress(10, 0, 0, 1));
  CHECK(server.gatewayIP() == IPAddress(10, 0, 0, 2));
  CHECK(client.localIP() == IPAddress(10, 0, 0, 2));
  CHECK(client.gatewayIP() == IPAddress(10, 0, 0, 1));
  CHECK(client.dnsIP(0) == IPAddress(8, 8, 8, 8));
  CHECK(client.dnsIP(1) == IPAddress(1, 1, 1, 1));
  CHECK(client.phase() == PPPPhase::Running);

  // ping the server: it answers automatically
  int replies = 0;
  client.onReceive([](const uint8_t *data, size_t len, void *ref) {
    if (len >= 28 && data[9] == 1 && data[20] == 0) (*(int *)ref)++;
  }, &replies);
  auto ping = icmpEcho(toIp4(10, 0, 0, 2), toIp4(10, 0, 0, 1), 1);
  CHECK(client.send(ping.data(), ping.size()));
  CHECK(runUntil([&] { return replies == 1; }, {&server, &client}));

  // large packet with characters that must be escaped
  std::vector<uint8_t> big(1500);
  for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)i;
  big[0] = 0x45;
  big[9] = 17;
  struct BigCheck {
    std::vector<uint8_t> *big;
    size_t received = 0;
  } check{&big};
  server.onReceive([](const uint8_t *data, size_t len, void *ref) {
    BigCheck &c = *(BigCheck *)ref;
    if (len == c.big->size() && memcmp(data, c.big->data(), len) == 0)
      c.received++;
  }, &check);
  size_t &received = check.received;
  CHECK(client.send(big.data(), big.size()));
  CHECK(runUntil([&] { return received == 1; }, {&server, &client}));

  // client terminates: both go down
  client.end();
  CHECK(!client.connected());
  CHECK(runUntil([&] { return !server.connected(); }, {&server, &client}));

  // reconnect
  client.begin(sb);
  CHECK(runUntil([&] { return server.connected() && client.connected(); },
                 {&server, &client}));
  CHECK(client.localIP() == IPAddress(10, 0, 0, 2));
}

/// A scripted peer that behaves like Linux pppd
class ScriptedPeer {
 public:
  LoopbackStream io;
  HDLCEncoder enc;
  HDLCDecoder<1504> dec;
  struct Frame {
    uint16_t proto;
    std::vector<uint8_t> data;
  };
  std::vector<Frame> received;

  ScriptedPeer() { enc.setOutput(&io); }

  void send(uint16_t proto, std::vector<uint8_t> pkt) {
    enc.begin(proto, true);
    enc.data(pkt.data(), pkt.size());
    enc.end();
  }

  void sendCP(uint16_t proto, uint8_t code, uint8_t id, std::vector<uint8_t> data) {
    std::vector<uint8_t> pkt = {code, id, 0, (uint8_t)(data.size() + 4)};
    pkt.insert(pkt.end(), data.begin(), data.end());
    send(proto, pkt);
  }

  void poll() {
    while (io.available()) {
      if (dec.add(io.read())) {
        uint8_t *f = dec.frame();
        Frame fr;
        fr.proto = (uint16_t)(f[2] << 8 | f[3]);
        fr.data.assign(f + 4, f + dec.frameLen());
        received.push_back(fr);
      }
    }
  }

  /// Finds the next received frame with the protocol and code
  bool take(uint16_t proto, uint8_t code, Frame &out) {
    poll();
    for (size_t i = 0; i < received.size(); i++) {
      if (received[i].proto == proto && received[i].data[0] == code) {
        out = received[i];
        received.erase(received.begin() + i);
        return true;
      }
    }
    return false;
  }
};

static void testPppdLikePeer() {
  printf("testPppdLikePeer\n");
  LoopbackStream sa;
  ScriptedPeer peer;
  sa.connect(peer.io);
  TinyPPP ppp;
  ppp.setCredentials("user", "secret");
  ppp.begin(sa);
  ScriptedPeer::Frame f;

  auto wait = [&](uint16_t proto, uint8_t code) {
    return runUntil([&] { return peer.take(proto, code, f); }, {&ppp}, 3000);
  };

  // our LCP Configure-Request: ACCM + magic
  CHECK(wait(PPP_LCP, CONF_REQ));
  CHECK(f.data.size() == 4 + 12);
  CHECK(f.data[4] == LCP_ACCM && f.data[10] == LCP_MAGIC);
  peer.sendCP(PPP_LCP, CONF_ACK, f.data[1],
              std::vector<uint8_t>(f.data.begin() + 4, f.data.end()));

  // pppd request: ACCM 0, auth PAP, magic, PFC, ACFC
  std::vector<uint8_t> opts = {2, 6, 0, 0, 0, 0, 3, 4, 0xC0, 0x23,
                               5, 6, 1, 2, 3, 4, 7, 2, 8, 2};
  peer.sendCP(PPP_LCP, CONF_REQ, 1, opts);
  CHECK(wait(PPP_LCP, CONF_ACK));
  CHECK(f.data[1] == 1);

  // PAP authentication request
  CHECK(wait(PPP_PAP, 1));
  CHECK(f.data[4] == 4 && memcmp(&f.data[5], "user", 4) == 0);
  CHECK(f.data[9] == 6 && memcmp(&f.data[10], "secret", 6) == 0);
  peer.sendCP(PPP_PAP, 2, f.data[1], {0});

  // IPCP request: address 0.0.0.0 + DNS
  CHECK(wait(PPP_IPCP, CONF_REQ));
  CHECK(f.data[4] == IPCP_ADDRESS && f.data[10] == IPCP_DNS1 &&
        f.data[16] == IPCP_DNS2);
  // Nak with the address to use and the DNS servers
  peer.sendCP(PPP_IPCP, CONF_NAK, f.data[1],
              {3, 6, 192, 168, 7, 2, 129, 6, 9, 9, 9, 9, 131, 6, 8, 8, 4, 4});
  CHECK(wait(PPP_IPCP, CONF_REQ));
  CHECK(f.data[6] == 192 && f.data[9] == 2);
  peer.sendCP(PPP_IPCP, CONF_ACK, f.data[1],
              std::vector<uint8_t>(f.data.begin() + 4, f.data.end()));

  // pppd IPCP request with VJ compression: must be rejected
  peer.sendCP(PPP_IPCP, CONF_REQ, 7, {2, 6, 0, 0x2d, 0x0f, 0x01, 3, 6, 192, 168, 7, 1});
  CHECK(wait(PPP_IPCP, CONF_REJ));
  CHECK(f.data[1] == 7 && f.data[4] == 2);
  peer.sendCP(PPP_IPCP, CONF_REQ, 8, {3, 6, 192, 168, 7, 1});
  CHECK(wait(PPP_IPCP, CONF_ACK));

  CHECK(runUntil([&] { return ppp.connected(); }, {&ppp}));
  CHECK(ppp.localIP() == IPAddress(192, 168, 7, 2));
  CHECK(ppp.gatewayIP() == IPAddress(192, 168, 7, 1));
  CHECK(ppp.dnsIP(0) == IPAddress(9, 9, 9, 9));
  CHECK(ppp.dnsIP(1) == IPAddress(8, 8, 4, 4));

  // CCP is not supported: Protocol-Reject
  peer.sendCP(0x80FD, CONF_REQ, 1, {});
  CHECK(wait(PPP_LCP, PROT_REJ));
  CHECK(f.data[4] == 0x80 && f.data[5] == 0xFD);

  // LCP echo
  peer.sendCP(PPP_LCP, ECHO_REQ, 5, {1, 2, 3, 4});
  CHECK(wait(PPP_LCP, ECHO_REP));
  CHECK(f.data[1] == 5);

  // compressed frames (ACFC + PFC) are accepted
  ppp.setRespondToPing(true);
  {
    auto ping = icmpEcho(toIp4(192, 168, 7, 1), toIp4(192, 168, 7, 2), 9);
    std::vector<uint8_t> frame = {0x21};
    frame.insert(frame.end(), ping.begin(), ping.end());
    uint16_t fcs = ~fcs16(frame.data(), frame.size());
    frame.push_back(fcs & 0xFF);
    frame.push_back(fcs >> 8);
    std::vector<uint8_t> raw = {0x7E};
    for (auto b : frame) {
      if (b == 0x7E || b == 0x7D || b < 0x20) {
        raw.push_back(0x7D);
        raw.push_back(b ^ 0x20);
      } else {
        raw.push_back(b);
      }
    }
    raw.push_back(0x7E);
    peer.io.clear();
    peer.received.clear();
    sa.clear();
    peer.io.write(raw.data(), raw.size());
  }
  CHECK(wait(PPP_IP, 0x45));
  CHECK(f.data.size() == 36 && f.data[20] == 0);  // echo reply

  // peer terminates
  peer.sendCP(PPP_LCP, TERM_REQ, 9, {});
  CHECK(wait(PPP_LCP, TERM_ACK));
  CHECK(!ppp.connected());
}

static void testPassiveAndIpcpServer() {
  printf("testPassiveAndIpcpServer\n");
  LoopbackStream sa;
  ScriptedPeer peer;
  sa.connect(peer.io);
  TinyPPP ppp;
  ppp.setPassive(true);
  ppp.config(IPAddress(10, 1, 0, 1), IPAddress(10, 1, 0, 2));
  ppp.setRequestDNS(false);
  ppp.begin(sa);
  ScriptedPeer::Frame f;
  auto wait = [&](uint16_t proto, uint8_t code) {
    return runUntil([&] { return peer.take(proto, code, f); }, {&ppp}, 3000);
  };

  // passive: nothing is sent
  runUntil([] { return false; }, {&ppp}, 100);
  peer.poll();
  CHECK(peer.received.empty());

  // a lwIP like client
  peer.sendCP(PPP_LCP, CONF_REQ, 1, {5, 6, 9, 9, 9, 9});
  CHECK(wait(PPP_LCP, CONF_REQ));
  peer.sendCP(PPP_LCP, CONF_ACK, f.data[1],
              std::vector<uint8_t>(f.data.begin() + 4, f.data.end()));
  CHECK(wait(PPP_LCP, CONF_ACK));
  CHECK(wait(PPP_IPCP, CONF_REQ));
  CHECK(f.data.size() == 10);  // only the address
  peer.sendCP(PPP_IPCP, CONF_ACK, f.data[1],
              std::vector<uint8_t>(f.data.begin() + 4, f.data.end()));
  // client asks for an address and DNS
  peer.sendCP(PPP_IPCP, CONF_REQ, 2, {3, 6, 0, 0, 0, 0, 129, 6, 0, 0, 0, 0});
  CHECK(wait(PPP_IPCP, CONF_REJ));  // no DNS to offer
  peer.sendCP(PPP_IPCP, CONF_REQ, 3, {3, 6, 0, 0, 0, 0});
  CHECK(wait(PPP_IPCP, CONF_NAK));
  CHECK(f.data[6] == 10 && f.data[7] == 1 && f.data[9] == 2);
  peer.sendCP(PPP_IPCP, CONF_REQ, 4, {3, 6, 10, 1, 0, 2});
  CHECK(wait(PPP_IPCP, CONF_ACK));
  CHECK(runUntil([&] { return ppp.connected(); }, {&ppp}));
  CHECK(ppp.localIP() == IPAddress(10, 1, 0, 1));
  CHECK(ppp.gatewayIP() == IPAddress(10, 1, 0, 2));
}

/// Two connected TinyPPP instances that poll each other while waiting
struct LinkedPair {
  LoopbackStream sa, sb;
  TinyPPP server, client;

  bool begin() {
    sa.connect(sb);
    server.config(IPAddress(10, 0, 0, 1), IPAddress(10, 0, 0, 2),
                  IPAddress(10, 0, 0, 1));
    server.setRequestDNS(false);
    server.begin(sa);
    client.begin(sb);
    server.onIdle([](void *ref) { ((TinyPPP *)ref)->loop(); }, &client);
    client.onIdle([](void *ref) { ((TinyPPP *)ref)->loop(); }, &server);
    return runUntil([&] { return server.connected() && client.connected(); },
                    {&server, &client});
  }
};

static void testTcp() {
  printf("testTcp\n");
  LinkedPair p;
  CHECK(p.begin());
  PPPServer listener(80, p.server);
  listener.begin();
  CHECK((bool)listener);

  // connect and accept
  PPPClient c(p.client);
  CHECK(c.connect(IPAddress(10, 0, 0, 1), 80) == 1);
  CHECK(c.connected());
  CHECK(c.remoteIP() == IPAddress(10, 0, 0, 1));
  CHECK(c.remotePort() == 80);
  PPPClient s;
  CHECK(runUntil([&] { return (bool)(s = listener.accept()); },
                 {&p.server, &p.client}));
  CHECK(s.remoteIP() == IPAddress(10, 0, 0, 2));
  CHECK(s.remotePort() == c.localPort());
  CHECK(!listener.accept());  // only once

  // transfer more data than the buffers hold in both directions
  const size_t total = 20000;
  std::vector<uint8_t> rxS, rxC;
  uint8_t chunk[300];
  size_t sent = 0;
  while (sent < total || rxS.size() < total || rxC.size() < total) {
    if (sent < total) {
      size_t n = total - sent < sizeof(chunk) ? total - sent : sizeof(chunk);
      for (size_t i = 0; i < n; i++) chunk[i] = (uint8_t)((sent + i) * 7);
      size_t a = c.write(chunk, n);
      for (size_t i = 0; i < n; i++) chunk[i] = (uint8_t)((sent + i) * 13);
      size_t b = s.write(chunk, n);
      CHECK(a == n && b == n);
      if (a != n || b != n) break;
      sent += n;
    }
    uint8_t buf[512];
    int n;
    while ((n = s.read(buf, sizeof(buf))) > 0) rxS.insert(rxS.end(), buf, buf + n);
    while ((n = c.read(buf, sizeof(buf))) > 0) rxC.insert(rxC.end(), buf, buf + n);
    p.client.loop();
    p.server.loop();
    static uint32_t start = millis();
    if (millis() - start > 20000) break;
  }
  CHECK(rxS.size() == total && rxC.size() == total);
  bool okS = true, okC = true;
  for (size_t i = 0; i < rxS.size(); i++) okS &= rxS[i] == (uint8_t)(i * 7);
  for (size_t i = 0; i < rxC.size(); i++) okC &= rxC[i] == (uint8_t)(i * 13);
  CHECK(okS && okC);

  // data sent right before the close is still delivered
  c.print("bye");
  c.stop();
  CHECK(!c);
  String rest;
  CHECK(runUntil([&] {
    while (s.available()) rest += (char)s.read();
    return !s.connected();
  }, {&p.server, &p.client}));
  CHECK(rest == "bye");
  s.stop();
  CHECK(runUntil([&] { return !s; }, {&p.server, &p.client}));

  // closed port: connection refused
  PPPClient c2(p.client);
  c2.setConnectionTimeout(2000);
  uint32_t start = millis();
  CHECK(c2.connect(IPAddress(10, 0, 0, 1), 81) == 0);
  CHECK(millis() - start < 1000);

  // the link goes down: connections are reset
  PPPClient c3(p.client);
  CHECK(c3.connect(IPAddress(10, 0, 0, 1), 80) == 1);
  PPPClient s3;
  CHECK(runUntil([&] { return (bool)(s3 = listener.accept()); },
                 {&p.server, &p.client}));
  p.client.end();
  CHECK(runUntil([&] { return !p.server.connected(); }, {&p.server, &p.client}));
  CHECK(!c3.connected());
  CHECK(!s3.connected());
  c3.stop();
  s3.stop();
}

static void testServerReset() {
  printf("testServerReset\n");
  LinkedPair p;
  CHECK(p.begin());
  PPPServer listener(8080, p.server);
  listener.begin();
  // fill all sockets, then closing them frees them again
  for (int round = 0; round < 3; round++) {
    PPPClient clients[TINYPPP_TCP_SOCKETS / 2];
    for (auto &c : clients) {
      c = PPPClient(p.client);
      CHECK(c.connect(IPAddress(10, 0, 0, 1), 8080) == 1);
    }
    for (size_t i = 0; i < TINYPPP_TCP_SOCKETS / 2; i++) {
      PPPClient s;
      CHECK(runUntil([&] { return (bool)(s = listener.accept()); },
                     {&p.server, &p.client}));
      s.print("hello");
      s.stop();
    }
    for (auto &c : clients) {
      String text;
      CHECK(runUntil([&] {
        while (c.available()) text += (char)c.read();
        return !c.connected();
      }, {&p.server, &p.client}));
      CHECK(text == "hello");
      c.stop();
    }
  }
}

static void testUdpAndDns() {
  printf("testUdpAndDns\n");
  LinkedPair p;
  CHECK(p.begin());
  CHECK(p.client.dnsIP(0) == IPAddress(10, 0, 0, 1));

  PPPUDP echo(p.server), u(p.client);
  CHECK(echo.begin(7) == 1);
  CHECK(u.begin(5000) == 1);
  CHECK(u.beginPacket(IPAddress(10, 0, 0, 1), 7) == 1);
  u.print("ping");
  CHECK(u.endPacket() == 1);
  int len = 0;
  CHECK(runUntil([&] { return (len = echo.parsePacket()) > 0; },
                 {&p.server, &p.client}));
  CHECK(len == 4);
  CHECK(echo.remoteIP() == IPAddress(10, 0, 0, 2));
  CHECK(echo.remotePort() == 5000);
  char buf[16] = {0};
  CHECK(echo.read(buf, sizeof(buf)) == 4);
  CHECK(strcmp(buf, "ping") == 0);
  echo.beginPacket(echo.remoteIP(), echo.remotePort());
  echo.print("pong");
  CHECK(echo.endPacket() == 1);
  CHECK(runUntil([&] { return (len = u.parsePacket()) > 0; },
                 {&p.server, &p.client}));
  memset(buf, 0, sizeof(buf));
  CHECK(u.read(buf, sizeof(buf)) == 4 && strcmp(buf, "pong") == 0);
  CHECK(u.parsePacket() == 0);

  // DNS: a minimal server on the other side answers A queries
  PPPUDP dns(p.server);
  CHECK(dns.begin(53) == 1);
  struct DnsServer {
    LinkedPair *p;
    PPPUDP *dns;
  } ctx{&p, &dns};
  p.client.onIdle([](void *ref) {
    DnsServer &c = *(DnsServer *)ref;
    PPPUDP &dns = *c.dns;
    c.p->server.loop();
    int n = dns.parsePacket();
    if (n <= 0) return;
    uint8_t q[512];
    n = dns.read(q, sizeof(q));
    q[2] |= 0x80;  // response
    q[7] = 1;      // one answer
    const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4,
                              93, 184, 216, 34};
    dns.beginPacket(dns.remoteIP(), dns.remotePort());
    dns.write(q, n);
    dns.write(answer, sizeof(answer));
    dns.endPacket();
  }, &ctx);
  IPAddress ip;
  CHECK(p.client.hostByName("example.com", ip));
  CHECK(ip == IPAddress(93, 184, 216, 34));
  CHECK(p.client.hostByName("10.1.2.3", ip));
  CHECK(ip == IPAddress(10, 1, 2, 3));
  CHECK(!p.client.hostByName("bad..name", ip, 500));
}

int main() {
  testFcs();
  testHdlc();
  testTwoEngines();
  testPppdLikePeer();
  testPassiveAndIpcpServer();
  testTcp();
  testServerReset();
  testUdpAndDns();
  if (failures) {
    printf("%d check(s) FAILED\n", failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
