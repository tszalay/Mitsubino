#pragma once

#include <LittleFS.h>

// Change this to add fields, it will automatically get propagated to config page etc.
#define FOR_ALL_FIELDS(FUN) \
  FUN(ssid) \
  FUN(password) \
  FUN(my_hostname) \
  FUN(mqtt_hostname) \
  FUN(mqtt_port) \
  FUN(mqtt_username) \
  FUN(mqtt_password)

struct PersistentData {
#define MEMBER_HELPER(X) String X;
  FOR_ALL_FIELDS(MEMBER_HELPER)
#undef MEMBER_HELPER
#define COUNTER_HELPER(X) 1+
  static constexpr size_t NumFields = FOR_ALL_FIELDS(COUNTER_HELPER) 0;
#undef COUNTER_HELPER

#define STRING_HELPER(X) #X,
static constexpr std::array<const char*, NumFields> FieldNames = {
  FOR_ALL_FIELDS(STRING_HELPER)
};
#undef STRING_HELPER

private:
  Logger* logger_;

public:

  PersistentData(Logger* logger) : logger_(logger) {}

  String* fields() {
    return (String*)this;
  }

  bool load() {
    LittleFS.begin();
    bool success = true;
    for (size_t i=0; i<NumFields; i++) {
      String name(FieldNames[i]);
      File f = LittleFS.open("/" + name, "r");
      if (!f) {
        logger_->println("File ", name, " could not be read");
        success = false;
      }
      fields()[i] = f.readString();
    }
    LittleFS.end();
    return success;
  }

  bool save() {
#ifdef ESP8266
    LittleFS.begin();
#else
    // format if necessary here
    LittleFS.begin(true);
#endif
    for (int i = 0; i < NumFields; i++) {
      String name = String(FieldNames[i]);
      File f = LittleFS.open("/" + name, "w");
      if (!f) {
        logger_->println("File ", name, " could not be opened for write");
        return false;
      }
      f.print(fields()[i]);
    }
    LittleFS.end();
    return true;
  }

  void print() {
    for (int i = 0; i < NumFields; i++)
      logger_->println(FieldNames[i], " = ", fields()[i]);
  }
};

