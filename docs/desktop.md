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

`build/examples/desktop/ppp-pty` runs the engine on a pseudo terminal, so you can connect the real pppd to it and ping it:

```bash
./build/examples/desktop/ppp-pty     # prints e.g. "pty: /dev/pts/5" (macOS: "pty: /dev/ttys005")
sudo pppd /dev/pts/5 115200 192.168.77.1:192.168.77.2 noauth local nodetach novj noccp
ping 192.168.77.2
```

The GitHub workflow [desktop.yml](../.github/workflows/desktop.yml) builds and runs the tests on Ubuntu and macOS.
