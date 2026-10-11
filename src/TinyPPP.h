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
 * - All other 32-bit cores, e.g. SAMD and mbed based WiFiNINA boards
 *   (Nano 33 IoT, MKR WiFi 1010, Nano RP2040 Connect), UNO R4 and the desktop
 *   (Arduino-Emulator): the TinyPPP engine with a small built-in TCP/IP stack
 *   (TCP, UDP, ICMP echo, DNS) that provides PPPClient, PPPServer and PPPUDP.
 *
 * On ESP32 and RP2040 PPPClient, PPPServer and PPPUDP are aliases for the
 * native classes, so the same sketch builds on all platforms.
 *
 * Define PPP_PREFER_GENERIC (before the include or as build flag) to use the
 * generic implementation on ESP32 and RP2040 as well.
 */

#if defined(ARDUINO_ARCH_AVR) || defined(ARDUINO_ARCH_MEGAAVR)
#error "TinyPPP needs a 32-bit board (AVR does not have enough RAM)"
#elif defined(PPP_PREFER_GENERIC)
#include "tinyppp/TinyPPP_Generic.h"
#elif defined(ESP32)
#include "tinyppp/TinyPPP_ESP32.h"
#elif defined(ARDUINO_ARCH_RP2040) && !defined(ARDUINO_ARCH_MBED)
#include "tinyppp/TinyPPP_RP2040.h"
#else
#include "tinyppp/TinyPPP_Generic.h"
#endif

// The public API in the global namespace; the internals stay in tinyppp
#ifndef PPP_USE_NAMESPACE
#define PPP_USE_NAMESPACE true
#endif

#if PPP_USE_NAMESPACE
using namespace tinyppp;
#endif

