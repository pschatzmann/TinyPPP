#pragma once
/**
 * TinyPPP: header only PPP (Point-to-Point Protocol) networking over any
 * Arduino Stream (USB serial, UART, ...).
 *
 * - ESP32 (all variants): uses the PPP implementation of ESP-IDF lwIP via
 *   esp_netif and integrates into the Arduino Network API (NetworkInterface,
 *   NetworkClient, NetworkServer, NetworkUDP, Network.onEvent()).
 * - RP2040 / RP2350 (Earle Philhower core): uses the TinyPPP engine on a
 *   lwIP netif and integrates with WiFiClient, WiFiServer, WiFiUDP, ...
 * - Desktop (Arduino-Emulator) or other cores: PPP engine with a raw IPv4
 *   packet API.
 *
 * ESP32 and RP2040 provide the portable aliases PPPClient, PPPServer and
 * PPPUDP.
 */

#if defined(ESP32)
#include "tinyppp/TinyPPP_ESP32.h"
#elif defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED)
#include "tinyppp/TinyPPP_RP2040.h"
#else
#include "tinyppp/TinyPPP_Desktop.h"
#endif
