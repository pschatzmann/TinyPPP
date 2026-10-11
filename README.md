# TinyPPP

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://cmake.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

TinyPPP is a header-only Arduino library for IP networking with PPP (Point-to-Point Protocol). It runs over any Arduino `Stream`, such as USB serial, a hardware UART or anything else that implements `Stream`. The library plugs into the network API of the Arduino core, so the usual client, server and UDP classes work over the PPP link.

| Platform | Implementation | Network classes |
|---|---|---|
| ESP32, S2, S3, C3, C6, P4 (arduino-esp32 3.x) | Native ESP-IDF lwIP PPP through `esp_netif`, with `TinyPPP` as a `NetworkInterface` | `NetworkClient`, `NetworkServer`, `NetworkUDP`, `Network.onEvent(ARDUINO_EVENT_PPP_*)` |
| RP2040 / RP2350 ([arduino-pico](https://github.com/earlephilhower/arduino-pico)) | TinyPPP engine on a lwIP netif | `WiFiClient`, `WiFiServer`, `WiFiUDP`, `HTTPClient`, ... |
| Desktop: Linux, macOS ([Arduino-Emulator](https://github.com/pschatzmann/Arduino-Emulator)) | TinyPPP engine with a raw IPv4 packet API | – |

On ESP32 and RP2040, `PPPClient`, `PPPServer` and `PPPUDP` are aliases for the native classes, so the same sketch builds on both platforms.

## Documentation

- [Usage](docs/usage.md): example sketch and API
- [Host Setup](docs/host-setup.md): running pppd on Linux or macOS, internet access via NAT
- [Platform Notes](docs/platform-notes.md): ESP32, RP2040 / RP2350 and memory
- [Desktop Build and Tests](docs/desktop.md): building and testing with the Arduino-Emulator
- [Testing Plan](docs/testing-plan.md): test levels, coverage gaps and the release checklist


## Installation

For Arduino, you can download the library as zip and call include Library -> zip library. Or you can git clone this project into the Arduino libraries folder e.g. with

```
cd  ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/TinyPPP.git
```

This project can also be built and executed on your desktop (Linux, macOS) with cmake (see [Desktop Build and Tests](docs/desktop.md)):

```
cd TinyPPP
mkdir build
cd build
cmake ..
make
```

