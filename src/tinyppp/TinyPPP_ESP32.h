#pragma once
/**
 * ESP32 implementation (arduino-esp32 3.x): uses the PPP support of the
 * ESP-IDF lwIP via esp_netif with a custom I/O driver that reads and writes
 * an Arduino Stream. TinyPPP is a NetworkInterface, so NetworkClient,
 * NetworkServer, NetworkUDP, Network.onEvent() (ARDUINO_EVENT_PPP_*) and
 * Network.hostByName() work as with WiFi or Ethernet.
 *
 * ESP-IDF runs PPP in client mode: the peer (e.g. Linux pppd) must assign
 * the IP addresses.
 *
 * Note: TinyPPP uses the PPP interface slot of the Network library, so it
 * can't be combined with the PPP (modem) library of the core.
 */
#include <Arduino.h>
#include <Network.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef TINYPPP_RX_BUFFER
#define TINYPPP_RX_BUFFER 512
#endif

/// Portable aliases for the network classes that work over PPP
using PPPClient = NetworkClient;
using PPPServer = NetworkServer;
using PPPUDP = NetworkUDP;

class TinyPPP : public NetworkInterface {
 public:
  TinyPPP() = default;
  TinyPPP(const TinyPPP &) = delete;
  TinyPPP &operator=(const TinyPPP &) = delete;
  ~TinyPPP() { end(); }

  /**
   * Starts PPP over the indicated stream (e.g. Serial, Serial1). The stream
   * must already be opened. A FreeRTOS task reads the stream in the
   * background. Returns immediately: use connected()/waitConnected().
   */
  bool begin(Stream &io, UBaseType_t taskPriority = 5,
             BaseType_t core = tskNO_AFFINITY) {
    if (_esp_netif != nullptr) end();
    _io = &io;

    Network.begin();

    if (esp_event_handler_instance_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                            &pppEventCb, this,
                                            &_pppEventInstance) != ESP_OK) {
      log_e("register NETIF_PPP_STATUS failed");
      return false;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_PPP();
    _esp_netif = esp_netif_new(&cfg);
    if (_esp_netif == nullptr) {
      log_e("esp_netif_new failed");
      end();
      return false;
    }
    // receive the IP events of the Network library
    initNetif(ESP_NETIF_ID_PPP);

    _driver.base.post_attach = postAttach;
    _driver.self = this;
    if (esp_netif_attach(_esp_netif, &_driver) != ESP_OK) {
      log_e("esp_netif_attach failed");
      end();
      return false;
    }

    if (_user.length() > 0) {
      esp_netif_ppp_set_auth(_esp_netif, NETIF_PPP_AUTHTYPE_PAP, _user.c_str(),
                             _password.c_str());
    }

    _running = true;
    _stopping = false;
    _restartAt = 0;
    if (xTaskCreatePinnedToCore(rxTask, "TinyPPP", 4096, this, taskPriority,
                                &_task, core) != pdPASS) {
      log_e("task creation failed");
      end();
      return false;
    }

    setStatusBits(ESP_NETIF_STARTED_BIT);
    postEvent(ARDUINO_EVENT_PPP_START);
    esp_netif_action_start(_esp_netif, nullptr, 0, nullptr);
    return true;
  }

  /// Terminates the link and removes the interface
  void end() {
    if (_esp_netif != nullptr && _running) {
      _stopping = true;
      xEventGroupClearBits(_events(), DEAD_BIT);
      esp_netif_action_stop(_esp_netif, nullptr, 0, nullptr);
      // the reader task must run to receive the Terminate-Ack
      xEventGroupWaitBits(_events(), DEAD_BIT, pdFALSE, pdTRUE,
                          pdMS_TO_TICKS(5000));
    }
    if (_task != nullptr) {
      _running = false;
      for (int i = 0; i < 100 && _task != nullptr; i++) delay(10);
    }
    if (connected()) postEvent(ARDUINO_EVENT_PPP_DISCONNECTED);
    if (_esp_netif != nullptr) {
      clearStatusBits(ESP_NETIF_STARTED_BIT | ESP_NETIF_CONNECTED_BIT |
                      ESP_NETIF_HAS_IP_BIT | ESP_NETIF_HAS_LOCAL_IP6_BIT |
                      ESP_NETIF_HAS_GLOBAL_IP6_BIT);
      postEvent(ARDUINO_EVENT_PPP_STOP);
      destroyNetif();
    }
    if (_pppEventInstance != nullptr) {
      esp_event_handler_instance_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID,
                                            _pppEventInstance);
      _pppEventInstance = nullptr;
    }
  }

  /// PAP credentials, if the peer requires authentication (before begin())
  void setCredentials(const char *user, const char *password) {
    _user = user ? user : "";
    _password = password ? password : "";
  }

  /// Debug output of the PPP state changes, e.g. setLogger(Serial1)
  void setLogger(Print &out) { _log = &out; }

  /// Waits until we have got an IP address
  bool waitConnected(uint32_t timeoutMs = 30000) {
    if (_esp_netif == nullptr) return false;
    return (waitStatusBits(ESP_NETIF_HAS_IP_BIT, timeoutMs) &
            ESP_NETIF_HAS_IP_BIT) != 0;
  }

  /// true when the link is up and we have an IP address
  bool connected() const { return _esp_netif != nullptr && hasIP(); }

 protected:
  static constexpr EventBits_t DEAD_BIT = BIT0;

  struct Driver {
    esp_netif_driver_base_t base;  // must be the first member
    TinyPPP *self;
  };

  Driver _driver{};
  Stream *_io = nullptr;
  Print *_log = nullptr;
  TaskHandle_t _task = nullptr;
  esp_event_handler_instance_t _pppEventInstance = nullptr;
  EventGroupHandle_t _eventGroup = nullptr;
  volatile bool _running = false;
  volatile bool _stopping = false;
  volatile uint32_t _restartAt = 0;
  String _user;
  String _password;

  EventGroupHandle_t _events() {
    if (_eventGroup == nullptr) _eventGroup = xEventGroupCreate();
    return _eventGroup;
  }

  size_t printDriverInfo(Print &out) const override {
    return out.print(",TinyPPP");
  }

  static void postEvent(arduino_event_id_t id) {
    arduino_event_t event;
    event.event_id = id;
    Network.postEvent(&event);
  }

  // esp_netif driver: called by esp_netif_attach()
  static esp_err_t postAttach(esp_netif_t *netif, void *handle) {
    Driver *driver = (Driver *)handle;
    driver->base.netif = netif;
    esp_netif_driver_ifconfig_t ifconfig = {};
    ifconfig.handle = handle;
    ifconfig.transmit = transmit;
    esp_err_t rc = esp_netif_set_driver_config(netif, &ifconfig);
    if (rc != ESP_OK) return rc;
    esp_netif_ppp_config_t pppConfig;
    esp_netif_ppp_get_params(netif, &pppConfig);
    pppConfig.ppp_phase_event_enabled = true;
    pppConfig.ppp_error_event_enabled = true;
    return esp_netif_ppp_set_params(netif, &pppConfig);
  }

  // esp_netif driver: output of the PPP stack (called from the lwIP task)
  static esp_err_t transmit(void *handle, void *buffer, size_t len) {
    TinyPPP *self = ((Driver *)handle)->self;
    if (self->_io == nullptr) return ESP_FAIL;
    const uint8_t *data = (const uint8_t *)buffer;
    size_t done = 0;
    for (int retry = 0; retry < 100 && done < len; retry++) {
      size_t n = self->_io->write(data + done, len - done);
      done += n;
      if (n == 0) vTaskDelay(1);
    }
    return done == len ? ESP_OK : ESP_FAIL;
  }

  // reads the stream and feeds the PPP stack
  static void rxTask(void *arg) {
    TinyPPP *self = (TinyPPP *)arg;
    uint8_t buffer[TINYPPP_RX_BUFFER];
    while (self->_running) {
      int avail = self->_io->available();
      if (avail > 0) {
        size_t n = self->_io->readBytes(
            buffer, (size_t)avail < sizeof(buffer) ? (size_t)avail : sizeof(buffer));
        if (n > 0) esp_netif_receive(self->_esp_netif, buffer, n, nullptr);
      } else {
        vTaskDelay(1);
      }
      // restart the negotiation after the link was lost
      uint32_t restart = self->_restartAt;
      if (restart != 0 && !self->_stopping &&
          (int32_t)(millis() - restart) >= 0) {
        self->_restartAt = 0;
        self->log("restarting");
        esp_netif_action_start(self->_esp_netif, nullptr, 0, nullptr);
      }
    }
    self->_task = nullptr;
    vTaskDelete(nullptr);
  }

  void log(const char *msg, int32_t id = -1) {
    log_d("TinyPPP: %s (%d)", msg, (int)id);
    if (_log == nullptr) return;
    _log->print("PPP: ");
    _log->print(msg);
    if (id >= 0) {
      _log->print(" (");
      _log->print(id);
      _log->print(")");
    }
    _log->println();
  }

  static void pppEventCb(void *arg, esp_event_base_t base, int32_t id,
                         void *data) {
    TinyPPP *self = (TinyPPP *)arg;
    // event data is a esp_netif_t** (except for NETIF_PPP_CONNECT_FAILED where
    // ESP-IDF posts the netif struct itself: there is only one PPP slot anyway)
    if (id != NETIF_PPP_CONNECT_FAILED) {
      esp_netif_t *netif = data ? *(esp_netif_t **)data : nullptr;
      if (netif != self->_esp_netif) return;
    }
    self->onPppEvent(id);
  }

  void onPppEvent(int32_t id) {
    if (id == NETIF_PPP_ERRORNONE) {
      log("connected");
      if (!(getStatusBits() & ESP_NETIF_CONNECTED_BIT)) {
        setStatusBits(ESP_NETIF_CONNECTED_BIT);
        postEvent(ARDUINO_EVENT_PPP_CONNECTED);
      }
    } else if (id < NETIF_PP_PHASE_OFFSET) {
      // error: the link is down
      log("link down, error", id);
      if (getStatusBits() & ESP_NETIF_CONNECTED_BIT) {
        clearStatusBits(ESP_NETIF_CONNECTED_BIT);
        postEvent(ARDUINO_EVENT_PPP_DISCONNECTED);
      }
      if (_stopping) {
        xEventGroupSetBits(_events(), DEAD_BIT);
      } else {
        _restartAt = millis() + 1000;
      }
    } else if (id == NETIF_PPP_PHASE_DEAD) {
      log("phase dead");
      if (_stopping) xEventGroupSetBits(_events(), DEAD_BIT);
    } else if (id == NETIF_PPP_PHASE_RUNNING) {
      log("phase running");
    } else if (id == NETIF_PPP_PHASE_ESTABLISH) {
      log("phase establish");
    } else if (id == NETIF_PPP_PHASE_NETWORK) {
      log("phase network");
    } else if (id == NETIF_PPP_CONNECT_FAILED) {
      log("connect failed");
      if (!_stopping) _restartAt = millis() + 2000;
    }
  }
};
