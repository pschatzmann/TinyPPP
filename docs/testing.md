# Testing Plan

This plan describes how TinyPPP is tested. It lists what the tests cover today, the gaps, and what to check before a release.

## Test Levels

| Level | What runs | Where | Automated |
|---|---|---|---|
| 1. Unit tests | `PPPEngine`, HDLC framing and the desktop `TinyPPP` with in-memory streams (`tests/LoopbackStream.h`) | Desktop (Arduino-Emulator) | Yes, `ctest` in [desktop.yml](../.github/workflows/desktop.yml) |
| 2. Interop tests | `ppp-pty` connected to the real Linux `pppd` | Linux (CI runner or local) | Planned |
| 3. Compile tests | The `examples/` sketches built for ESP32 and RP2040 boards | `arduino-cli` | Planned |
| 4. Hardware tests | Sketches on real boards connected to `pppd` on a computer | Bench | Manual |

Levels 1 to 3 run on every push. Level 4 runs before each release and after changes to a platform file (`TinyPPP_ESP32.h`, `TinyPPP_RP2040.h`).

## 1. Unit Tests

### Covered today (`tests/test_tinyppp.cpp`)

- FCS-16 check value, HDLC escaping, a corrupted frame is dropped and counted
- Two engines: negotiation, addresses and DNS, ping, a 1500-byte packet, terminate and reconnect
- A scripted pppd-like peer: PAP, IPCP Nak with DNS, VJ reject, CCP Protocol-Reject, LCP echo, ACFC/PFC frames, peer terminate
- Passive mode and the IPCP server role (address Nak, DNS reject)

### Cases to add

To reach most of these code paths, add a way to advance time: either a `processTimers(now)` call with a fake clock, or a short `TINYPPP_RESTART_MS` set for the test build.

**HDLC (`HDLC.h`)**
- With ACCM `0`, control characters are not escaped. With ACCM `0xFFFFFFFF`, all of them are. LCP frames always escape them.
- A frame larger than `MAX_FRAME` is dropped, and the frame after it is still decoded.
- Runt frames (fewer than 4 bytes), repeated flags and an escape byte right before a flag are ignored.
- When the output accepts only part of a write, `end()` returns `false` and `stats().txErrors` goes up.

**LCP**
- The MRU option is accepted and limited to `TINYPPP_MRU`. An MRU option with the wrong length is rejected.
- Unknown options get a Configure-Reject that contains only the unknown options.
- Auth = CHAP gets a Nak that proposes PAP.
- If the peer sends our own magic number (looped-back line), we answer with a Nak and a new magic number.
- If the peer rejects ACCM or the magic number, our next request leaves it out. If the peer Naks the magic number, we pick a new one.
- Malformed packets are ignored and the state does not change: length < 4, length > frame, option length < 2, an option that runs past the end.
- An unknown code gets a Code-Reject.
- Retransmission: with no answer, requests are sent again. After 10 retries, the interval grows to `5 × TINYPPP_RESTART_MS`, and the engine never gives up.
- Renegotiation: a Configure-Request in the Opened state takes the IP link down and brings it back up.
- Echo keep-alive: with `setEchoInterval()` set, Echo-Requests are sent. A reply resets the failure count. After `maxFailures` missed replies, LCP restarts.

**PAP**
- An Authenticate-Nak leads to a Terminate-Request and the Terminate phase.
- No response after 10 retries closes the link.
- A reply with the wrong id is ignored.
- A 63-character user name and password are sent unchanged. Longer values are cut off and nothing overflows.

**IPCP**
- The peer rejects the address option and we have no static address: `ipcpUp()` fails and LCP closes.
- No IPCP answer after 10 retries closes LCP.
- Server role with offered DNS: a request for `0.0.0.0` gets a Nak with the offered value, and the correct value gets an Ack.
- IPCP Terminate-Request: the IP link goes down and IPCP restarts while LCP stays up.
- An IPCP packet that arrives before LCP is open is dropped and counted in `rxDropped`.
- The peer Protocol-Rejects IPCP: IPCP stops.

**Data path and API**
- IP packets received before IPCP is up are counted in `rxDropped`. `send()` before the link is up returns `false`.
- `stats()` counters match the frames exchanged.
- Calling `end()` in each LCP state, calling `end()` twice, and calling `begin()` again after `end()` all work.
- Passive mode where the peer never starts: no bytes are sent and nothing is logged.
- Timer wraparound: all timers still fire when `millis()` wraps past `0xFFFFFFFF`.
- The desktop ping reply has valid IP and ICMP checksums (check them independently) and handles an IP header with options (IHL > 5). It does not answer pings addressed to other hosts or packets longer than `TINYPPP_MRU`.

### Robustness

- Build the tests with `-fsanitize=address,undefined` as an extra CI job.
- Add a libFuzzer target that feeds random bytes to `PPPEngine::input()` in every phase, including byte-stuffed frames with a valid FCS so that the fuzzer gets past framing. Run it for a short time in CI and longer locally.

## 2. Interop Tests with pppd

Script these with `ppp-pty` and `PPP_TIMEOUT` on the Ubuntu runner (`sudo apt-get install ppp`, passwordless `sudo` is available):

| Case | pppd options | Pass criteria |
|---|---|---|
| Basic link | `192.168.77.1:192.168.77.2 noauth local nodetach novj noccp` | `connected:` line, `ping -c 5 192.168.77.2` gets 0 % loss |
| Large packets | as above | `ping -s 1472 -M do` succeeds |
| DNS | add `ms-dns 8.8.8.8 ms-dns 1.1.1.1` | `ppp-pty` prints `dns 8.8.8.8` |
| PAP | `require-pap`, `user` + `/etc/ppp/pap-secrets`, set the credentials in `ppp-pty` (needs an env var) | link comes up. With a wrong password, pppd reports an authentication failure. |
| Default pppd (VJ, CCP on) | leave out `novj noccp` | link comes up (VJ rejected, CCP protocol-rejected) |
| Keep-alive | `lcp-echo-interval 1 lcp-echo-failure 3` | link stays up for 60 s |
| Peer restart | kill pppd, start it again | `disconnected`, then `connected` again with no restart of `ppp-pty` |
| Engine restart | stop `ppp-pty` (PPP_TIMEOUT), start it again | pppd reconnects (with `persist`) |

At the end, check the summary line printed by `ppp-pty`: `fcs errors 0`.

## 3. Compile Tests

Add a workflow that builds `examples/WebServer` and `examples/HttpClient` with `arduino-cli compile --warnings all` for:

- `esp32:esp32:esp32` (classic, UART only)
- `esp32:esp32:esp32s3` (USB OTG CDC)
- `esp32:esp32:esp32c3` (USB Serial/JTAG)
- `rp2040:rp2040:rpipico`
- `rp2040:rp2040:rpipico2` (RP2350)

Pin the core versions (arduino-esp32 3.x, arduino-pico) so that a breaking core release shows up as a separate change.

## 4. Hardware Tests

### Boards

| Board | PPP stream | Notes |
|---|---|---|
| ESP32-S3 | USB CDC (`Serial`) | main ESP32 target |
| ESP32-C3 | USB Serial/JTAG | different USB driver |
| ESP32 classic | `Serial1` UART + USB-UART adapter | no native USB |
| Raspberry Pi Pico | USB CDC and `Serial1` | test both streams |
| Raspberry Pi Pico 2 | USB CDC | RP2350 |

Connect a second UART to read the debug output from `setLogger()`. Use Linux as the main host and macOS for a smoke test.

### Cases (per board)

1. **WebServer**: `curl http://192.168.7.2` 1000 times in a loop. Expect no failures and steady response times.
2. **HttpClient through NAT**: the DNS lookup of `example.com` works and the HTTP response arrives.
3. **Throughput**: download a large response (for example 1 MB from a sketch-side generator) and note the KB/s. On RP2040, compare `setPollingPeriod(20)` with `setPollingPeriod(2)`, and on UART try 115200 and 921600 baud with `setFIFOSize(4096)`.
4. **Link loss**: stop pppd, wait, then start it again. The board reconnects without a reset. ESP32 posts `ARDUINO_EVENT_PPP_DISCONNECTED` and then `_CONNECTED`. RP2040 calls `onStateChange(false/true)`.
5. **USB unplug/replug** while a transfer is running: no crash, and the board reconnects.
6. **Board reset** while pppd is running with `persist`: the link comes back up.
7. **`end()` / `begin()` cycles**: 100 cycles. Free heap (`ESP.getFreeHeap()`, `rp2040.getFreeHeap()`) is the same after each cycle. On ESP32, `end()` returns within 5 s.
8. **PAP** with pppd `require-pap`, with the right and the wrong password.
9. **Soak test**: 24 h with `lcp-echo-interval 10` and periodic HTTP requests. Expect no disconnects and no growth in heap use or `fcsErrors()`.
10. **RP2040 only**: the static `config()` server role, `setDefault(false)` together with WiFi on a Pico W (traffic goes out on the right interface), and `status()` values.

Write down the results for each release: board, core version, host OS, pass/fail and throughput.

## Release Checklist

- [ ] Unit tests pass on Ubuntu and macOS, and the sanitizer job is clean
- [ ] pppd interop tests pass
- [ ] Examples compile for all boards in section 3
- [ ] Hardware cases 1, 2, 4 and 7 pass on at least one ESP32 and one RP2040 board
- [ ] Soak test run if the engine or a platform file changed
- [ ] [desktop.md](desktop.md) updated if the test coverage changed
