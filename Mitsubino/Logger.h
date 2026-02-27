#pragma once

#include <Arduino.h>

template <size_t N>
struct FixedString {
  std::array<char, N> data;

  FixedString() : data{} {}

  constexpr FixedString(std::string_view s) : data{} {
    std::copy(s.begin(), s.begin() + std::min(s.size(), N-1), data.begin());
  }

  FixedString(const String& s) : data{} {
    memcpy(data.begin(), s.begin(), std::min(s.length(), N-1));
  }

  FixedString& operator=(String s) {
    memset(data.begin(), 0, sizeof(data));
    memcpy(data.begin(), s.begin(), std::min(s.length(), N-1));
    return *this;
  }

  operator String() const {
    return String(data.begin(), N);
  }

  constexpr auto operator<=>(const FixedString&) const = default;
};

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

  template<size_t N>
  void print_impl(const FixedString<N>& fs) {
    print_impl(fs.data.begin());
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
