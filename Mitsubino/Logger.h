#pragma once

#include <Arduino.h>

class Logger {
  String buffer_;
  const size_t capacity_;
  bool use_serial_{false};

  template<typename T>
  void print_impl(const T& t) {
    String s(t);
    if (s.length() + buffer_.length() >= (capacity_-1)) {
      clear();
      buffer_.concat("-- truncated --\n");
    }
    buffer_.concat(s);
    if (use_serial_)
      Serial.print(s);
  }

  template<typename T, typename... U>
  void print_impl(const T& t, const U&... u) {
    print_impl(t);
    print_impl(u...);
  }

public:
  Logger(size_t capacity) : capacity_(capacity) {
    buffer_.reserve(capacity);
  }

  void set_serial(bool use_serial) {
    use_serial_ = use_serial;
  }

  void clear() {
    buffer_.remove(0, buffer_.length());
  }

  const String& get() {
    return buffer_;
  }

  template<typename... T>
  void print(const T&... t) {
    print_impl(t...);
  }

  template<typename... T>
  void println(const T&... t) {
    print(millis(), ": ", t..., "\n");
  }
};

inline String mac2str(const uint8_t* mac) {
  char macStr[18];
  sprintf(macStr, MACSTR, MAC2STR(mac));
  return String(macStr);
}
