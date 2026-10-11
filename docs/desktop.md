# Desktop Build and Tests

The engine can be built and tested on the desktop (Linux and macOS) with the Arduino-Emulator. You need CMake and a C++17 compiler (on macOS: the Xcode command line tools, `xcode-select --install`, and CMake, e.g. `brew install cmake`):

```bash
cmake -B build                       # downloads the Arduino-Emulator
# or use a local checkout:
cmake -B build -DFETCHCONTENT_SOURCE_DIR_ARDUINO_EMULATOR=~/Development/Arduino-Emulator
cmake --build build
ctest --test-dir build
```

The tests cover:
- FCS and HDLC framing
- two engines negotiating with each other, then exchanging ping, large packets, terminate and reconnect
- a scripted pppd-like peer: PAP, an IPCP Nak with DNS, a VJ reject, a CCP protocol-reject, echo, compressed frames and terminate
- the passive/server role
- the built-in TCP/IP stack between two instances: TCP connect, accept, a 20 KB transfer in both directions, close, connection refused and link loss; reuse of sockets; UDP; DNS

The desktop examples in [examples/desktop](../examples/desktop) are regular Arduino sketches (`setup()` and `loop()`). They are built with `arduino_sketch()` from the Arduino-Emulator's `Arduino.cmake`, and the emulator provides `main()`.

`build/examples/desktop/ppp-pty` ([ppp-pty.ino](../examples/desktop/ppp-pty/ppp-pty.ino)) runs PPP over `Serial`, which is stdin/stdout in the emulator. With its `pty` option, pppd starts the sketch on a pseudo terminal, so you can connect the real pppd to it and ping it:

```bash
cd build/examples/desktop
sudo pppd pty "./ppp-pty" 192.168.77.1:192.168.77.2 noauth local nodetach novj noccp nodefaultroute
ping 192.168.77.2
```

pppd runs the command with the rights of the user who started pppd. Because stdout carries PPP, the sketch writes its log to stderr. Set `PPP_TIMEOUT` (seconds) to stop it automatically, e.g. `pty "env PPP_TIMEOUT=60 ./ppp-pty"`.

The GitHub workflow [desktop.yml](../.github/workflows/desktop.yml) builds and runs the tests on Ubuntu and macOS.
