#pragma once

class Logger {
  String buffer_;
  const size_t capacity_;

  template<typename T>
  void print_impl(const T& t) {
    String s(t);
    if (s.length() + buffer_.length() >= (capacity_-1)) {
      clear();
      buffer_.concat("-- truncated --\n");
    }
    buffer_.concat(s);
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
