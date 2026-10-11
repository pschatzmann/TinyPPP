/**
 * Desktop sketch (Arduino-Emulator, Linux, macOS): PPP over Serial, which is
 * stdin/stdout in the emulator. pppd starts the sketch on a pseudo terminal
 * with its pty option. The stack answers ping.
 *
 *   sudo pppd pty "./ppp-pty" 192.168.77.1:192.168.77.2 noauth local \
 *        nodetach novj noccp nodefaultroute
 *   ping 192.168.77.2
 *
 * Serial carries PPP, so log output goes to stderr. Set PPP_TIMEOUT (seconds)
 * to stop automatically, e.g. for automated tests.
 */
#include "TinyPPP.h"

#include <unistd.h>

/// Print to stderr: stdout is used by PPP
class StderrPrint : public Print {
 public:
  size_t write(uint8_t b) override { return fputc(b, stderr) == EOF ? 0 : 1; }
};

StderrPrint errorLog;
TinyPPP ppp;
uint32_t timeoutMs = 0;
pid_t parentPid = 0;

void onStateChange(bool up, void *) {
  if (up) {
    fprintf(stderr, "connected: local %s peer %s dns %s\n",
            ppp.localIP().toString().c_str(),
            ppp.gatewayIP().toString().c_str(),
            ppp.dnsIP().toString().c_str());
  } else {
    fprintf(stderr, "disconnected\n");
  }
}

void setup() {
  const char *env = getenv("PPP_TIMEOUT");
  timeoutMs = env ? atoi(env) * 1000 : 0;
  parentPid = getppid();

  Serial.begin(115200);
  ppp.setLogger(errorLog);
  ppp.onStateChange(onStateChange);
  ppp.begin(Serial);
}

void loop() {
  ppp.loop();
  delay(1);

  // pppd has terminated: we were re-parented
  if (getppid() != parentPid) exit(0);

  if (timeoutMs != 0 && millis() >= timeoutMs) {
    const auto &s = ppp.stats();
    fprintf(stderr, "rx frames %u ip %u, tx frames %u ip %u, fcs errors %u\n",
            s.rxFrames, s.rxIP, s.txFrames, s.txIP, ppp.fcsErrors());
    ppp.end();
    exit(0);
  }
}
