# Testing

This document describes how TinyPPP is tested. It lists the current status, what the tests cover, the gaps, and what to check before a release.

## Test Levels

| Level | What runs | Where | Automated |
|---|---|---|---|
| 1. Unit tests | `PPPEngine`, HDLC framing, the generic `TinyPPP` and its IP stack (`IPStack.h`, `PPPClient`, `PPPServer`, `PPPUDP`) with in-memory streams (`tests/LoopbackStream.h`) | Desktop (Arduino-Emulator) | Yes, `ctest` in [desktop.yml](../.github/workflows/desktop.yml) |
| 2. Interop tests | `ppp-pty` connected to the real Linux `pppd` | Linux (CI runner or local) | Planned |
| 3. Compile tests | The `examples/` sketches built for ESP32, RP2040 and generic boards | `arduino-cli` | Run by hand, workflow planned |
| 4. Hardware tests | Sketches on real boards connected to `pppd` on a computer | Bench | Manual |

The goal is that levels 1 to 3 run on every push. Level 4 runs before each release and after changes to a platform file (`TinyPPP_ESP32.h`, `TinyPPP_RP2040.h`, `TinyPPP_Generic.h`) or to the IP stack (`IPStack.h`, `PPPSockets.h`).

## Status

As of 2026-10-11:

| Level | Status |
|---|---|
| 1. Unit tests | 8 tests pass on Linux: FCS, HDLC, two engines, pppd-like peer, passive/server role, and the IP stack tests `testTcp`, `testServerReset` and `testUdpAndDns`. The build has no warnings with `-Wall -Wextra`. The [desktop.yml](../.github/workflows/desktop.yml) workflow runs them on Ubuntu and macOS. |
| 2. Interop tests | Not automated yet. |
| 3. Compile tests | Run by hand with `arduino-cli`: `WebServer` and `HttpClient` build without warnings for `arduino:samd:nano_33_iot`, `arduino:samd:mkrwifi1010` and `arduino:mbed_nano:nanorp2040connect`. `WebServer` builds without warnings for `arduino:renesas_uno:unor4wifi`, `rp2040:rp2040:rpipico` and `esp32:esp32:esp32`. `arduino:avr:uno` stops with the `#error` of `TinyPPP.h`. There's no workflow yet. |
| 4. Hardware tests | Not run yet for the generic stack (Nano 33 IoT, MKR WiFi 1010, Nano RP2040 Connect). |

## 1. Unit Tests

### Covered today (`tests/test_tinyppp.cpp`)

- FCS-16 check value, HDLC escaping, a corrupted frame is dropped and counted
- Two engines: negotiation, addresses and DNS, ping, a 1500-byte packet, terminate and reconnect
- A scripted pppd-like peer: PAP, IPCP Nak with DNS, VJ reject, CCP Protocol-Reject, LCP echo, ACFC/PFC frames, peer terminate
- Passive mode and the IPCP server role (address Nak, DNS reject)
- IP stack, with two instances that poll each other through `onIdle()`:
  - TCP (`testTcp`): connect and accept, addresses and ports, `accept()` returns a connection only once, 20 KB in both directions with 1 KB buffers (content checked), data written right before `stop()` is delivered and the peer sees the close, a closed port refuses the connection at once, link loss resets the connections
  - Socket reuse (`testServerReset`): three rounds of connect, send, close in which the closed sockets are freed and reused
  - UDP and DNS (`testUdpAndDns`): echo with sender address and port, `hostByName()` against a small DNS server on the other instance, a dotted IP string without a lookup, an invalid name fails

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
- The ping reply of the IP stack has valid IP and ICMP checksums (check them independently) and handles an IP header with options (IHL > 5). It does not answer pings addressed to other hosts, pings with a wrong checksum or with `setRespondToPing(false)`.

**IP stack (`IPStack.h`, `PPPSockets.h`)**

Most of these cases need lost or reordered packets. Add a filter to `LoopbackStream` (or between the two instances) that can drop, delay or swap single frames.
- Retransmission: a lost data segment, a lost SYN, a lost SYN-ACK and a lost FIN are sent again, and the transfer completes.
- Out-of-order and duplicate segments are dropped and answered with a duplicate ACK. A segment that is partly old is trimmed.
- Zero window: the receiver doesn't read, the sender's buffer fills up, `write()` times out, and one-byte probes are sent. After the receiver reads, a window update restarts the transfer.
- After `TINYPPP_TCP_MAX_RETRIES` timeouts, the connection is reset and `connected()` returns `false`.
- TIME_WAIT and linger: an orphaned socket is freed after `TINYPPP_TCP_TIME_WAIT` and `TINYPPP_TCP_LINGER`. When all sockets are in use, a new connection takes over a socket in TIME_WAIT.
- All sockets in use: a SYN to a listening port is dropped and succeeds when the peer sends it again after a socket was freed.
- A segment with a wrong TCP or UDP checksum is dropped. A segment for an unknown connection gets a reset, but not when a raw `onReceive()` callback is set.
- A reset from the peer closes the connection; a connection that was not accepted yet is freed.
- `PPPServer::available()` returns only connections with data, and `PPPServer::write()` reaches all connected clients. `end()` stops accepting while open connections stay open.
- UDP: datagrams are dropped when the receive buffer is full, `parsePacket()` skips the unread rest of the previous datagram, `write()` beyond `TINYPPP_UDP_TX_BUFFER` makes `endPacket()` fail, a datagram larger than the MTU is not sent, and `begin()` on a port in use fails.
- DNS: a response with a wrong id, an error code, a CNAME before the A record or no A record at all, a timeout when no server answers, and the fallback to the second DNS server.
- IP: fragments and packets for other addresses are not handled by the stack and go to the `onReceive()` callback.

### Robustness

- Build the tests with `-fsanitize=address,undefined` as an extra CI job.
- Add a libFuzzer target that feeds random bytes to `PPPEngine::input()` in every phase, including byte-stuffed frames with a valid FCS so that the fuzzer gets past framing. Run it for a short time in CI and longer locally.

## 2. Interop Tests with pppd

Script these with `ppp-pty` (started by pppd with `pty "env PPP_TIMEOUT=60 ./ppp-pty"`, its log goes to stderr) on the Ubuntu runner (`sudo apt-get install ppp`, passwordless `sudo` is available):

| Case | pppd options | Pass criteria |
|---|---|---|
| Basic link | `pty "./ppp-pty" 192.168.77.1:192.168.77.2 noauth local nodetach novj noccp` | `connected:` line, `ping -c 5 192.168.77.2` gets 0 % loss |
| Large packets | as above | `ping -s 1472 -M do` succeeds |
| DNS | add `ms-dns 8.8.8.8 ms-dns 1.1.1.1` | `ppp-pty` prints `dns 8.8.8.8` |
| PAP | `require-pap`, `user` + `/etc/ppp/pap-secrets`, set the credentials in `ppp-pty` (needs an env var) | link comes up. With a wrong password, pppd reports an authentication failure. |
| Default pppd (VJ, CCP on) | leave out `novj noccp` | link comes up (VJ rejected, CCP protocol-rejected) |
| Keep-alive | `lcp-echo-interval 1 lcp-echo-failure 3` | link stays up for 60 s |
| Peer exit | kill pppd | `ppp-pty` exits as well (no process left behind) |
| Engine restart | stop `ppp-pty` (PPP_TIMEOUT), start it again | pppd reconnects (with `persist`) |

At the end, check the summary line printed by `ppp-pty`: `fcs errors 0`.

## 3. Compile Tests

Add a workflow that builds `examples/WebServer` and `examples/HttpClient` with `arduino-cli compile --warnings all` for:

- `esp32:esp32:esp32` (classic, UART only)
- `esp32:esp32:esp32s3` (USB OTG CDC)
- `esp32:esp32:esp32c3` (USB Serial/JTAG)
- `rp2040:rp2040:rpipico`
- `rp2040:rp2040:rpipico2` (RP2350)
- `arduino:samd:nano_33_iot` and `arduino:samd:mkrwifi1010` (generic stack, SAMD21 with 32 KB RAM, C++11)
- `arduino:mbed_nano:nanorp2040connect` (generic stack on mbed)
- `arduino:renesas_uno:unor4wifi` (generic stack)
- `arduino:avr:uno`: the build must fail with the `#error` message of `TinyPPP.h`

For the generic boards, also note the RAM use that `arduino-cli` prints, so that growth shows up. The numbers in [ram.md](ram.md) come from these builds.

Pin the core versions (arduino-esp32 3.x, arduino-pico, Arduino SAMD, mbed and Renesas cores) so that a breaking core release shows up as a separate change.

## 4. Hardware Tests

### Boards

| Board | PPP stream | Notes |
|---|---|---|
| ESP32-S3 | USB CDC (`Serial`) | main ESP32 target |
| ESP32-C3 | USB Serial/JTAG | different USB driver |
| ESP32 classic | `Serial1` UART + USB-UART adapter | no native USB |
| Raspberry Pi Pico | USB CDC and `Serial1` | test both streams |
| Raspberry Pi Pico 2 | USB CDC | RP2350 |
| Arduino Nano 33 IoT or MKR WiFi 1010 | USB CDC (`Serial`) | generic stack, 32 KB RAM, WiFiNINA |
| Arduino Nano RP2040 Connect | USB CDC (`Serial`) | generic stack on mbed |

Connect a second UART to read the debug output from `setLogger()`. Use Linux as the main host and macOS for a smoke test.

### Cases (per board)

1. **WebServer**: `curl http://192.168.7.2` 1000 times in a loop. Expect no failures and steady response times.
2. **HttpClient through NAT**: the DNS lookup of `example.com` works and the HTTP response arrives.
3. **Throughput**: download a large response (for example 1 MB from a sketch-side generator) and note the KB/s. On RP2040, compare `setPollingPeriod(20)` with `setPollingPeriod(2)`, and on UART try 115200 and 921600 baud with `setFIFOSize(4096)`.
4. **Link loss**: stop pppd, wait, then start it again. The board reconnects without a reset. ESP32 posts `ARDUINO_EVENT_PPP_DISCONNECTED` and then `_CONNECTED`. RP2040 and the generic boards call `onStateChange(false/true)`.
5. **USB unplug/replug** while a transfer is running: no crash, and the board reconnects.
6. **Board reset** while pppd is running with `persist`: the link comes back up.
7. **`end()` / `begin()` cycles**: 100 cycles. Free heap (`ESP.getFreeHeap()`, `rp2040.getFreeHeap()`) is the same after each cycle. The generic stack uses only static memory, so there check that all sockets can still be used. On ESP32, `end()` returns within 5 s.
8. **PAP** with pppd `require-pap`, with the right and the wrong password.
9. **Soak test**: 24 h with `lcp-echo-interval 10` and periodic HTTP requests. Expect no disconnects and no growth in heap use or `fcsErrors()`.
10. **RP2040 only**: the static `config()` server role, `setDefault(false)` together with WiFi on a Pico W (traffic goes out on the right interface), and `status()` values.
11. **Generic boards only**: a sketch that calls only `ppp.loop()` and `delay()` for minutes between socket calls (the link must stay up and pings must be answered), a sketch with the reduced buffer settings from [ram.md](ram.md), several clients at the same time (up to `TINYPPP_TCP_SOCKETS`), and on a WiFiNINA board WiFi (`WiFiClient`) and PPP (`PPPClient`) used together.

Write down the results for each release: board, core version, host OS, pass/fail and throughput.

## Release Checklist

- [ ] Unit tests pass on Ubuntu and macOS, and the sanitizer job is clean
- [ ] pppd interop tests pass
- [ ] Examples compile for all boards in section 3
- [ ] Hardware cases 1, 2, 4 and 7 pass on at least one ESP32, one RP2040 and one generic board (e.g. Nano 33 IoT)
- [ ] Soak test run if the engine or a platform file changed
- [ ] [desktop.md](desktop.md) updated if the test coverage changed
