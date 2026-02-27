#pragma once

#ifdef ESP8266
#include <ESP8266WebServer.h>
#else
#include <WiFi.h>
#include <esp_wifi.h>
#endif

#include "States.h"
#include "Logger.h"

enum class WifiStates : int {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
};

class WifiClientStateMachine : public CRTPStateMachine<WifiClientStateMachine, WifiStates> {
  Logger* logger_;
public:
  using CRTPStateMachine::state_t;

  static constexpr const char* name = "Wifi";
  static constexpr state_t initial_state = state_t::DISCONNECTED;

  WifiClientStateMachine(Logger* logger, String hostname, String ssid, String password) : logger_(logger) {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G);
    WiFi.hostname(hostname);
    WiFi.begin(ssid, password);
  }

  bool connected() {
    return state() == state_t::CONNECTED;
  }

  void loopImpl() {
    switch (state()) {
      case state_t::CONNECTED:
        if (WiFi.status() != WL_CONNECTED) {
          transition(state_t::DISCONNECTED);
        }
        return;
      case state_t::CONNECTING:
        if (time_in_state() > 100 && WiFi.status() == WL_CONNECTED) {
          transition(state_t::CONNECTED);
          logger_->println("Connected to ", WiFi.SSID());
          logger_->println("IP address: ", WiFi.localIP().toString());
        }
        else if (time_in_state() > 120*1000) {
          ESP.restart(); // RIP
        }
        return;
      case state_t::DISCONNECTED:
        // if we just disconnected, give it some time first
        if (time_in_state() > 100) {
          WiFi.begin();
          transition(state_t::CONNECTING);
        }
        return;
    }
  }
};



