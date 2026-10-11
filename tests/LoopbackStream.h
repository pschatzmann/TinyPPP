#pragma once
#include <deque>

#include <Arduino.h>

/// In memory full duplex connection: what is written to one end can be read
/// from the other end.
class LoopbackStream : public Stream {
 public:
  void connect(LoopbackStream &peer) {
    _peer = &peer;
    peer._peer = this;
  }

  int available() override { return (int)_rx.size(); }
  int read() override {
    if (_rx.empty()) return -1;
    int b = _rx.front();
    _rx.pop_front();
    return b;
  }
  int peek() override { return _rx.empty() ? -1 : _rx.front(); }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    if (_peer == nullptr) return 0;
    if (_drop) return len;
    for (size_t i = 0; i < len; i++) _peer->_rx.push_back(data[i]);
    return len;
  }

  /// Simulate a disconnected line: written data is lost
  void setDrop(bool drop) { _drop = drop; }
  void clear() { _rx.clear(); }

 private:
  LoopbackStream *_peer = nullptr;
  std::deque<uint8_t> _rx;
  bool _drop = false;
};
