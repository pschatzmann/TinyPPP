#pragma once
/**
 * HDLC-like framing for PPP (RFC 1662): FCS-16, byte stuffing and frame
 * reassembly. Platform independent: only depends on the Arduino Print API.
 */
#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

namespace tinyppp {

static constexpr uint8_t HDLC_FLAG = 0x7E;
static constexpr uint8_t HDLC_ESCAPE = 0x7D;
static constexpr uint8_t HDLC_XOR = 0x20;
static constexpr uint8_t HDLC_ALL_STATIONS = 0xFF;
static constexpr uint8_t HDLC_UI = 0x03;
static constexpr uint16_t FCS16_INIT = 0xFFFF;
static constexpr uint16_t FCS16_GOOD = 0xF0B8;

/// Incremental FCS-16 (RFC 1662, reflected CRC-CCITT) using a nibble table
inline uint16_t fcs16Update(uint16_t fcs, uint8_t b) {
  static const uint16_t table[16] = {
      0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
      0x8408, 0x9489, 0xA50A, 0xB58B, 0xC60C, 0xD68D, 0xE70E, 0xF78F};
  fcs = (fcs >> 4) ^ table[(fcs ^ b) & 0x0F];
  fcs = (fcs >> 4) ^ table[(fcs ^ (b >> 4)) & 0x0F];
  return fcs;
}

/// FCS-16 over a buffer (continues from fcs)
inline uint16_t fcs16(const uint8_t *data, size_t len,
                      uint16_t fcs = FCS16_INIT) {
  for (size_t i = 0; i < len; i++) fcs = fcs16Update(fcs, data[i]);
  return fcs;
}

/**
 * @brief HDLC frame encoder (RFC 1662).
 *
 * Writes one HDLC frame at a time to a Print. Output is collected in a small
 * buffer so that the underlying (USB) serial gets reasonably sized writes.
 */
class HDLCEncoder {
 public:
  /// Defines the output (e.g. the serial port)
  void setOutput(Print *out) { _out = out; }

  /// Async-Control-Character-Map of the peer: control chars to escape
  void setACCM(uint32_t accm) { _accm = accm; }

  /// Starts a frame with address/control header and the protocol number
  void begin(uint16_t protocol, bool escapeAllControl = false) {
    _escapeAll = escapeAllControl;
    _fcs = FCS16_INIT;
    _len = 0;
    _ok = _out != nullptr;
    put(HDLC_FLAG);
    data(HDLC_ALL_STATIONS);
    data(HDLC_UI);
    data(protocol >> 8);
    data(protocol & 0xFF);
  }

  /// Adds one payload byte
  void data(uint8_t b) {
    _fcs = fcs16Update(_fcs, b);
    stuffed(b);
  }

  /// Adds payload bytes
  void data(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) data(buf[i]);
  }

  /// Appends FCS and closing flag; returns false if the output dropped bytes
  bool end() {
    uint16_t fcs = ~_fcs;
    stuffed(fcs & 0xFF);
    stuffed(fcs >> 8);
    put(HDLC_FLAG);
    flush();
    return _ok;
  }

 private:
  Print *_out = nullptr;
  uint32_t _accm = 0xFFFFFFFF;
  bool _escapeAll = true;
  bool _ok = false;
  uint16_t _fcs = FCS16_INIT;
  uint8_t _buf[256];
  size_t _len = 0;

  void stuffed(uint8_t b) {
    bool esc = b == HDLC_FLAG || b == HDLC_ESCAPE ||
               (b < 0x20 && (_escapeAll || (_accm & (1UL << b))));
    if (esc) {
      put(HDLC_ESCAPE);
      put(b ^ HDLC_XOR);
    } else {
      put(b);
    }
  }

  void put(uint8_t b) {
    if (_len == sizeof(_buf)) flush();
    _buf[_len++] = b;
  }

  void flush() {
    if (_len == 0) return;
    if (_out == nullptr) {
      // no output: drop the data (the buffer must be emptied in any case)
      _ok = false;
      _len = 0;
      return;
    }
    size_t written = 0;
    // some cores return partial writes: retry a few times
    for (int retry = 0; retry < 50 && written < _len; retry++) {
      written += _out->write(_buf + written, _len - written);
    }
    if (written < _len) _ok = false;
    _len = 0;
  }
};

/**
 * @brief HDLC frame decoder (RFC 1662).
 *
 * Reassembles frames from a byte stream. After a complete frame with a valid
 * FCS has been received, add() returns true and frame()/frameLen() give the
 * content (address/control removed when present, FCS removed).
 */
template <size_t MAX_FRAME>
class HDLCDecoder {
 public:
  /// Feed one byte: returns true when a valid frame is available
  bool add(uint8_t b) {
    if (b == HDLC_FLAG) {
      bool result = false;
      if (_len >= 4 && !_overflow) {
        if (fcs16(_buf, _len) == FCS16_GOOD) {
          result = true;
          _frameLen = _len - 2;
        } else {
          _fcsErrors++;
        }
      }
      _len = 0;
      _escape = false;
      _overflow = false;
      return result;
    }
    if (b == HDLC_ESCAPE) {
      _escape = true;
      return false;
    }
    if (_escape) {
      b ^= HDLC_XOR;
      _escape = false;
    }
    if (_len < sizeof(_buf)) {
      _buf[_len++] = b;
    } else {
      _overflow = true;
    }
    return false;
  }

  /// Frame content without FCS (address/control included if sent)
  uint8_t *frame() { return _buf; }
  /// Length of the frame returned by frame()
  size_t frameLen() const { return _frameLen; }
  /// Number of frames dropped because of a wrong FCS
  uint32_t fcsErrors() const { return _fcsErrors; }

 private:
  uint8_t _buf[MAX_FRAME + 2];
  size_t _len = 0;
  size_t _frameLen = 0;
  uint32_t _fcsErrors = 0;
  bool _escape = false;
  bool _overflow = false;
};

}  // namespace tinyppp
