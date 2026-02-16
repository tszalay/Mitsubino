#pragma once

#ifdef ESP8266
#include <ESP8266WebServer.h>
#else
#include <WiFi.h>
#endif

struct SimpleTimer {
  const int interval;
  unsigned long last_tick{0};
  SimpleTimer(int _interval) : interval(_interval) {}
  bool tick() {
    unsigned long time = millis();
    if (time-last_tick >= interval) {
      last_tick = time;
      return true;
    }
    return false;
  }
  // can use this as a timeout instead of a timer
  bool peek() {
    return millis() - last_tick >= interval;
  }
  void reset() {
    last_tick = millis();
  }
};

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

  int istate() const {
    return (int)state_;
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
  }

  void loop() {
    unsigned long curtime = millis();
    time_in_state_ += (curtime - last_tick_);
    last_tick_ = curtime;
    derived()->loopImpl();
  }
};
