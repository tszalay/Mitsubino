#pragma once

#include <WiFi.h>

template <typename T, typename STATES>
class CRTPStateMachine {
public:
  using state_t = STATES;

private:
  state_t state_;
  unsigned long last_tick_;
  uint64_t time_in_state_;

  T* derived() {
    return static_cast<T*>(this);
  }

public:
  CRTPStateMachine() : state_{T::initial_state}, last_tick_(millis()), time_in_state_(0) {}

  state_t state() const {
    return state_;
  }

  uint64_t time_in_state() {
    return time_in_state_;
  }

  void transition(state_t new_state) {
    if (new_state == state_)
      return;
    auto old_state = state_;
    state_ = new_state;
    time_in_state_ = 0;
    last_tick_ = millis();
    derived()->onTransition(old_state, new_state);
  }

  void loop() {
    unsigned long curtime = millis();
    time_in_state_ += (curtime - last_tick_);
    last_tick_ = curtime;
    derived()->loopImpl();
  }
};

enum class WifiStates : int {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
};

class WifiClientStateMachine : public CRTPStateMachine<WifiClientStateMachine, WifiStates> {
public:
  using CRTPStateMachine::state_t;

  WifiClientStateMachine(String hostname, String ssid, String password) {
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G);
    WiFi.hostname(hostname);
    WiFi.begin(ssid, password);
  }

  static constexpr state_t initial_state = state_t::CONNECTING;

  void onTransition(state_t oldstate, state_t newstate) {

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



