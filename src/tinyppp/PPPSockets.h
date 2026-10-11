#pragma once
/**
 * Arduino network classes (Client, Server, UDP) on the TinyPPP IPStack, for
 * platforms without lwIP. The classes use the stack of the TinyPPP instance
 * (the first one created) unless a stack is passed to the constructor.
 */
#include <Client.h>
#include <Server.h>
#include <Udp.h>

#include "IPStack.h"
#include "PPPEngine.h"

#ifndef TINYPPP_TIMEOUT
#define TINYPPP_TIMEOUT 5000
#endif

/**
 * @brief TCP client over PPP: used like WiFiClient or EthernetClient.
 *
 * Copies refer to the same connection; it is closed by stop() only.
 */
class PPPClient : public Client {
 public:
  /// Client on the default stack (the first TinyPPP instance)
  PPPClient() = default;
  /// Client on the indicated stack
  explicit PPPClient(tinyppp::IPStack &stack) : _stack(&stack) {}
  /// Client for an existing socket (used by PPPServer)
  PPPClient(tinyppp::IPStack *stack, int sock)
      : _stack(stack), _sock(sock), _gen(stack->tcpGen(sock)) {}

  /// Connects to the address and port: returns 1 on success
  int connect(IPAddress ip, uint16_t port) override {
    if (stack() == nullptr) return 0;
    if (valid()) stop();
    _sock = stack()->tcpConnect(tinyppp::toIp4(ip[0], ip[1], ip[2], ip[3]),
                                port, _timeout);
    if (_sock < 0) return 0;
    _gen = stack()->tcpGen(_sock);
    return 1;
  }

  /// Resolves the host via DNS and connects: returns 1 on success
  int connect(const char *host, uint16_t port) override {
    if (stack() == nullptr) return 0;
    IPAddress ip;
    if (!stack()->hostByName(host, ip, _timeout)) return 0;
    return connect(ip, port);
  }

  /// Sends one byte
  size_t write(uint8_t b) override { return write(&b, 1); }
  /// Sends data (waits while the send buffer is full)
  size_t write(const uint8_t *buf, size_t size) override {
    if (!valid()) return 0;
    size_t n = stack()->tcpWrite(_sock, buf, size, _timeout);
    if (n < size) setWriteError();
    return n;
  }
  using Print::write;

  /// Number of received bytes that can be read
  int available() override {
    return valid() ? stack()->tcpAvailable(_sock) : 0;
  }

  /// Reads one byte (-1 if none available)
  int read() override {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }

  /// Reads up to size bytes: returns the number read or -1
  int read(uint8_t *buf, size_t size) override {
    if (!valid()) return -1;
    int n = stack()->tcpRead(_sock, buf, size);
    return n > 0 ? n : -1;
  }

  /// Next byte without removing it (-1 if none)
  int peek() override { return valid() ? stack()->tcpPeek(_sock) : -1; }

  /// Waits until all written data was acknowledged by the peer
  void flush() override {
    if (valid()) stack()->tcpFlush(_sock, _timeout);
  }

  /// Closes the connection
  void stop() override {
    if (valid()) stack()->tcpClose(_sock, 1000);
    _sock = -1;
  }

  /// true while connected or while received data is available
  uint8_t connected() override {
    if (!valid()) return 0;
    if (stack()->tcpAvailable(_sock) > 0) return 1;
    return stack()->tcpState(_sock) == tinyppp::TCPState::Established;
  }

  /// true if the client refers to an open socket
  operator bool() override { return valid(); }

  /// true if both refer to the same connection
  bool operator==(const PPPClient &other) const {
    return _sock == other._sock && _gen == other._gen;
  }
  /// true if they refer to different connections
  bool operator!=(const PPPClient &other) const { return !(*this == other); }

  /// Address of the peer
  IPAddress remoteIP() {
    return valid() ? tinyppp::toIPAddress(stack()->tcpRemoteIP(_sock))
                   : IPAddress(0, 0, 0, 0);
  }
  /// Port of the peer
  uint16_t remotePort() {
    return valid() ? stack()->tcpRemotePort(_sock) : 0;
  }
  /// Our port
  uint16_t localPort() { return valid() ? stack()->tcpLocalPort(_sock) : 0; }

  /// Timeout in ms for connect, write and flush
  void setConnectionTimeout(uint32_t ms) { _timeout = ms; }

 protected:
  tinyppp::IPStack *_stack = nullptr;
  int _sock = -1;
  uint8_t _gen = 0;
  uint32_t _timeout = TINYPPP_TIMEOUT;

  tinyppp::IPStack *stack() const {
    return _stack ? _stack : tinyppp::IPStack::current();
  }
  bool valid() const {
    return _sock >= 0 && stack() != nullptr && stack()->tcpValid(_sock, _gen);
  }
};

/// @brief TCP server over PPP: used like WiFiServer or EthernetServer
class PPPServer : public Server {
 public:
  /// Server on the default stack (the first TinyPPP instance)
  explicit PPPServer(uint16_t port = 80) : _port(port) {}
  /// Server on the indicated stack
  PPPServer(uint16_t port, tinyppp::IPStack &stack)
      : _port(port), _stack(&stack) {}

  /// Starts listening
  void begin() override {
    if (stack()) _listening = stack()->tcpListen(_port);
  }
  /// Starts listening on the indicated port
  void begin(uint16_t port) {
    end();
    _port = port;
    begin();
  }

  /// Stops listening (open connections stay open)
  void end() {
    if (stack() && _listening) stack()->tcpUnlisten(_port);
    _listening = false;
  }

  /// Returns a new connection (or an invalid client)
  PPPClient accept() {
    int s = stack() ? stack()->tcpAccept(_port, false) : -1;
    return s < 0 ? PPPClient() : PPPClient(stack(), s);
  }

  /// Returns a connection that has data available (or an invalid client)
  PPPClient available() {
    int s = stack() ? stack()->tcpAccept(_port, true) : -1;
    return s < 0 ? PPPClient() : PPPClient(stack(), s);
  }

  /// Writes one byte to all connected clients
  size_t write(uint8_t b) override { return write(&b, 1); }
  /// Writes data to all connected clients
  size_t write(const uint8_t *buf, size_t size) override {
    return stack() ? stack()->tcpWriteAll(_port, buf, size, TINYPPP_TIMEOUT)
                   : 0;
  }
  using Print::write;

  /// true if the server is listening
  operator bool() { return _listening; }
  /// The server port
  uint16_t port() const { return _port; }

 protected:
  uint16_t _port;
  tinyppp::IPStack *_stack = nullptr;
  bool _listening = false;

  tinyppp::IPStack *stack() const {
    return _stack ? _stack : tinyppp::IPStack::current();
  }
};

/// @brief UDP socket over PPP: used like WiFiUDP or EthernetUDP
class PPPUDP : public UDP {
 public:
  /// UDP on the default stack (the first TinyPPP instance)
  PPPUDP() = default;
  /// UDP on the indicated stack
  explicit PPPUDP(tinyppp::IPStack &stack) : _stack(&stack) {}
  ~PPPUDP() { stop(); }
  PPPUDP(const PPPUDP &) = delete;
  PPPUDP &operator=(const PPPUDP &) = delete;

  /// Opens the socket on the port (0: any port): returns 1 on success
  uint8_t begin(uint16_t port) override {
    stop();
    if (stack() == nullptr) return 0;
    _sock = stack()->udpOpen(port);
    return _sock >= 0;
  }

  /// Closes the socket
  void stop() override {
    if (_sock >= 0 && stack()) stack()->udpClose(_sock);
    _sock = -1;
    _txLen = 0;
  }

  /// Starts a datagram to the address and port
  int beginPacket(IPAddress ip, uint16_t port) override {
    if (_sock < 0 && !begin(0)) return 0;
    _txIP = ip;
    _txPort = port;
    _txLen = 0;
    _txOverflow = false;
    return 1;
  }

  /// Starts a datagram to the host (resolved via DNS) and port
  int beginPacket(const char *host, uint16_t port) override {
    if (stack() == nullptr) return 0;
    IPAddress ip;
    if (!stack()->hostByName(host, ip)) return 0;
    return beginPacket(ip, port);
  }

  /// Sends the datagram: returns 1 on success
  int endPacket() override {
    if (_sock < 0 || _txOverflow) return 0;
    bool ok = stack()->udpSend(
        _sock, tinyppp::toIp4(_txIP[0], _txIP[1], _txIP[2], _txIP[3]), _txPort,
        _tx, _txLen);
    _txLen = 0;
    return ok;
  }

  /// Adds one byte to the datagram
  size_t write(uint8_t b) override { return write(&b, 1); }
  /// Adds data to the datagram (max TINYPPP_UDP_TX_BUFFER bytes)
  size_t write(const uint8_t *buf, size_t size) override {
    if (_txLen + size > sizeof(_tx)) {
      _txOverflow = true;
      size = sizeof(_tx) - _txLen;
    }
    memcpy(_tx + _txLen, buf, size);
    _txLen += size;
    return size;
  }
  using Print::write;

  /// Moves to the next received datagram: returns its size (0 if none)
  int parsePacket() override {
    if (_sock < 0) return 0;
    tinyppp::ip4_t ip = 0;
    int n = stack()->udpParsePacket(_sock, ip, _rxPort);
    if (n > 0 || stack()->udpAvailable(_sock) > 0) _rxIP = tinyppp::toIPAddress(ip);
    return n;
  }

  /// Unread bytes of the current datagram
  int available() override {
    return _sock < 0 ? 0 : stack()->udpAvailable(_sock);
  }

  /// Reads one byte of the current datagram (-1 if none)
  int read() override {
    uint8_t b;
    return read(&b, 1) == 1 ? b : -1;
  }
  /// Reads from the current datagram: returns the number read or -1
  int read(unsigned char *buf, size_t len) override {
    if (_sock < 0) return -1;
    int n = stack()->udpRead(_sock, buf, len);
    return n > 0 ? n : -1;
  }
  /// Reads from the current datagram: returns the number read or -1
  int read(char *buf, size_t len) override {
    return read((unsigned char *)buf, len);
  }

  /// Next byte of the current datagram without removing it (-1 if none)
  int peek() override { return _sock < 0 ? -1 : stack()->udpPeek(_sock); }

  /// Discards the rest of the current datagram
  void flush() override {
    if (_sock >= 0) stack()->udpDiscard(_sock);
  }

  /// Sender address of the current datagram
  IPAddress remoteIP() override { return _rxIP; }
  /// Sender port of the current datagram
  uint16_t remotePort() override { return _rxPort; }
  /// Our port
  uint16_t localPort() { return _sock < 0 ? 0 : stack()->udpLocalPort(_sock); }

 protected:
  tinyppp::IPStack *_stack = nullptr;
  int _sock = -1;
  IPAddress _rxIP;
  uint16_t _rxPort = 0;
  IPAddress _txIP;
  uint16_t _txPort = 0;
  size_t _txLen = 0;
  bool _txOverflow = false;
  uint8_t _tx[TINYPPP_UDP_TX_BUFFER];

  tinyppp::IPStack *stack() const {
    return _stack ? _stack : tinyppp::IPStack::current();
  }
};
