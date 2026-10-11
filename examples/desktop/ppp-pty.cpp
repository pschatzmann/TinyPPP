/**
 * Desktop demo (Linux, macOS): runs TinyPPP on a pseudo terminal so that the
 * real pppd can connect to it. The engine answers ping.
 *
 *   ./ppp-pty     # prints the pty name, e.g. /dev/pts/5 (macOS: /dev/ttys005)
 *   sudo pppd /dev/pts/5 115200 192.168.77.1:192.168.77.2 noauth local \
 *        nodetach novj noccp nodefaultroute
 *   ping 192.168.77.2
 */
// TinyPPP (Arduino.h) must come first: termios.h defines B0, B1, ... macros
#include "TinyPPP.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/// Arduino Stream on a file descriptor (non blocking)
class FdStream : public Stream {
 public:
  explicit FdStream(int fd) : _fd(fd) {}
  int available() override {
    fill();
    return _len - _pos;
  }
  int read() override {
    fill();
    return _pos < _len ? _buf[_pos++] : -1;
  }
  int peek() override {
    fill();
    return _pos < _len ? _buf[_pos] : -1;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    size_t done = 0;
    int waits = 0;
    while (done < len) {
      ssize_t n = ::write(_fd, data + done, len - done);
      if (n > 0) {
        done += n;
        waits = 0;
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                 waits++ < 100) {
        usleep(100);  // buffer full: give the peer a moment
      } else {
        // nobody is reading (e.g. pppd not started, EIO on macOS):
        // drop the data like a disconnected serial line would
        break;
      }
    }
    return len;
  }

 private:
  int _fd;
  uint8_t _buf[512];
  int _pos = 0, _len = 0;
  void fill() {
    if (_pos < _len) return;
    ssize_t n = ::read(_fd, _buf, sizeof(_buf));
    _pos = 0;
    _len = n > 0 ? (int)n : 0;
  }
};

int main() {
  int fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
    perror("pty");
    return 1;
  }
  struct termios tio;
  tcgetattr(fd, &tio);
  cfmakeraw(&tio);
  tcsetattr(fd, TCSANOW, &tio);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  printf("pty: %s\n", ptsname(fd));
  fflush(stdout);

  // optional timeout in seconds (for automated tests)
  const char *env = getenv("PPP_TIMEOUT");
  uint32_t timeout = env ? atoi(env) * 1000 : 0;

  FdStream io(fd);
  StdioDevice log;
  TinyPPP ppp;
  ppp.setLogger(log);
  ppp.onStateChange([&](bool up) {
    if (up) {
      printf("connected: local %s peer %s dns %s\n",
             ppp.localIP().toString().c_str(),
             ppp.gatewayIP().toString().c_str(),
             ppp.dnsIP().toString().c_str());
    } else {
      printf("disconnected\n");
    }
    fflush(stdout);
  });
  ppp.begin(io);

  uint32_t start = millis();
  while (timeout == 0 || millis() - start < timeout) {
    ppp.loop();
    delay(1);
  }
  const auto &s = ppp.stats();
  printf("rx frames %u ip %u, tx frames %u ip %u, fcs errors %u\n", s.rxFrames,
         s.rxIP, s.txFrames, s.txIP, ppp.fcsErrors());
  ppp.end();
  return 0;
}
