#pragma once
/**
 * TinyPPP: header only PPP (Point-to-Point Protocol) networking over any
 * Arduino Stream (USB serial, UART, ...).
 *
 * Implementations and their class names:
 * - TinyPPP_ESP32 (ESP32, all variants): ESP-IDF lwIP PPP via esp_netif,
 *   integrated into the Arduino Network API. Sockets: PPPClient_ESP32 (NetworkClient),
 *   PPPServer_ESP32 (NetworkServer), PPPUDP_ESP32 (NetworkUDP).
 * - TinyPPP_RP2040 (RP2040 / RP2350, Earle Philhower core): TinyPPP engine on
 *   a lwIP netif. Sockets: PPPClient_RP2040 (WiFiClient), PPPServer_RP2040
 *   (WiFiServer), PPPUDP_RP2040 (WiFiUDP).
 * - TinyPPP_Generic (every board, including the desktop): TinyPPP engine with
 *   a small built-in TCP/IP stack (TCP, UDP, ICMP echo, DNS). Sockets:
 *   PPPClient_Generic, PPPServer_Generic, PPPUDP_Generic.
 *
 * The names without a suffix (TinyPPP, PPPClient, PPPServer, PPPUDP) refer to
 * the implementation of the current platform. They are the default choice:
 * the same sketch builds on all platforms. Use the suffixed names to pick an
 * implementation explicitly, e.g. when both kinds are used in one sketch.
 *
 * Define PPP_PREFER_GENERIC (before the include or as build flag) to make the
 * generic implementation the default on ESP32 and RP2040.
 *
 * AVR is rejected as default because the generic stack does not fit in its
 * RAM. TinyPPP_Generic.h can still be included there with reduced buffer
 * sizes (see docs/ram.md) at your own risk.
 */

#if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_MEGAAVR)
#error "TinyPPP needs a 32-bit board (AVR does not have enough RAM)"
#endif

#include "tinyppp/TinyPPP_Generic.h"
#if !defined(PPP_PREFER_GENERIC)
#if defined(ESP32)
#include "tinyppp/TinyPPP_ESP32.h"
#elif defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED)
#include "tinyppp/TinyPPP_RP2040.h"
#endif
#endif

// Default implementation of this platform
namespace tinyppp {
#if defined(PPP_PREFER_GENERIC)
using TinyPPP = TinyPPP_Generic;
using PPPClient = PPPClient_Generic;
using PPPServer = PPPServer_Generic;
using PPPUDP = PPPUDP_Generic;
#elif defined(ESP32)
using TinyPPP = TinyPPP_ESP32;
using PPPClient = PPPClient_ESP32;
using PPPServer = PPPServer_ESP32;
using PPPUDP = PPPUDP_ESP32;
#elif defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED)
using TinyPPP = TinyPPP_RP2040;
using PPPClient = PPPClient_RP2040;
using PPPServer = PPPServer_RP2040;
using PPPUDP = PPPUDP_RP2040;
#else
using TinyPPP = TinyPPP_Generic;
using PPPClient = PPPClient_Generic;
using PPPServer = PPPServer_Generic;
using PPPUDP = PPPUDP_Generic;
#endif
}  // namespace tinyppp

// The public API in the global namespace; the internals stay in tinyppp
#ifndef PPP_USE_NAMESPACE
#define PPP_USE_NAMESPACE true
#endif

#if PPP_USE_NAMESPACE
using namespace tinyppp;
#endif
