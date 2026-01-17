// only use this code if we are compiling for the remote temp sensors

#ifdef ESP32

// Overall settings don't really matter, but to keep partition consistent:
// Use "default 4MB SPIFFS" partition if re-flashing, don't wipe
// Upload speed 921600, CPU 240MHz all seems to work

#include "MitsubinoShared.h"
#include <Adafruit_SHT4x.h> // 1.0.4

Adafruit_SHT4x sht4{};
SimpleTimer g_temp_timer{15000};
SimpleTimer g_mqtt_reconnect_timer{100};
int g_mqtt_reconnect_attempts = 0;

void handle_mqtt_message(char* topic, byte* payload, unsigned int length) {
  /*if (String(topic) != get_topic_name("control")) {
    g_logger.println("Received message at unrecognized topic: ", topic);
    return;
  }*/

  // this is probably fine
  String message;
  message.concat((const char*)payload, length);
  g_logger.println("Got MQTT message on topic ", topic, ": ", message);
  /*DynamicJsonDocument root(JSON_OBJECT_SIZE(6));
  DeserializationError error = deserializeJson(root, message.c_str());

  if (error) {
    g_logger.println("Error parsing received mqtt message: ", error.c_str());
    return;
  }

  if (root.containsKey("power"))
    g_heat_pump.setPowerSetting((const char*)root["power"]);
  if (root.containsKey("mode"))
    g_heat_pump.setModeSetting(root["mode"]);
  if (root.containsKey("temperature"))
    g_heat_pump.setTemperature(root["temperature"]);
  if (root.containsKey("fan"))
    g_heat_pump.setFanSpeed(root["fan"]);
  if (root.containsKey("vane"))
    g_heat_pump.setVaneSetting(root["vane"]);
  if (root.containsKey("wideVane"))
    g_heat_pump.setWideVaneSetting(root["wideVane"]);
  if (root.containsKey("remoteTemp"))
    g_heat_pump.setRemoteTemperature(root["remoteTemp"]);

  g_heat_pump.update();
  g_logger.println("Updated heat pump");*/
}

#ifdef SDA1
#define WIRE_TO_USE Wire1
#define SDA_TO_USE SDA1
#define SCL_TO_USE SCL1
#else
#define WIRE_TO_USE Wire
#define SDA_TO_USE SDA
#define SCL_TO_USE SCL
#endif

void setup() {
  Serial.begin(115200);
  // connect to wifi, mDNS, OTA, MQTT, start server, etc
  configure_shared();
  WIRE_TO_USE.setPins(SDA_TO_USE, SCL_TO_USE);
  WiFi.setSleep(false);
}

void read_sensor() {
  g_logger.println("Free RAM: ", ESP.getFreeHeap());
  float uptime = millis();
  uptime /= (1000 * 60 * 60 * 24);
  g_logger.println("Uptime: ", uptime, " days");
  if (!sht4.begin(&WIRE_TO_USE)) {
    g_logger.println("Couldn't find SHT4x");
  }
  else {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    g_logger.println(F("SHT4x sensor connected and set to high precision"));
  }

  sensors_event_t humidity, temp;
  bool success = sht4.getEvent(&humidity, &temp);
  if (!success) {
    g_logger.println("Failed to read temp sensor");
    g_reset_timer.reset();
    return;
  }
  
  {
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(2));
    msg["temperature"] = temp.temperature;
    msg["humidity"] = humidity.relative_humidity;

    String s;
    serializeJson(msg, s);

    // don't retain these readings, so that the heat pump unit can fallback to
    // internal thermostat if they stop sending for some reason
    if (!g_mqtt_client.publish(get_topic_name("reading").c_str(), s.c_str(), false)) {
      g_logger.println("Failed to publish sensor readings to reading topic");
    }
    else {
      g_logger.println("sent reading: ", s);
      g_reset_timer.reset();
    }
  }
  if (g_persistent_data[PFIELD::my_hostname] == "remote_temp_1")
  {
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(1));
    msg["remoteTemp"] = temp.temperature;
    String s;
    serializeJson(msg, s);
    g_logger.println("sending control setting: ", s);
    if (!g_mqtt_client.publish("heatpumps/hp_livingroom/control", s.c_str(), false))
      g_logger.println("Failed to publish sensor readings to living room control topic");
    if (!g_mqtt_client.publish("heatpumps/hp_kitchen/control", s.c_str(), false))
      g_logger.println("Failed to publish sensor readings to kitchen control topic");
  }

}

void loop() {
  loop_shared();

  // crappy ad-hoc disconnect state machine. ESP8266 is much better at this.
  if (WiFi.status() != WL_CONNECTED) {
    g_logger.println("WiFi disconnected, attempting reconnect");
    WiFi.disconnect();
    WiFi.reconnect();
    g_logger.println("Reconnect attempt complete, current status ", WiFi.status());
    for (int i=0; i<10; i++) {
      if (WiFi.status() == WL_CONNECTED)
        break;
      delay(1000);
    }
    return;
  }

  g_reset_timer.reset();

  /*if (g_mqtt_client.connected()) {
    g_mqtt_reconnect_attempts = 0;
    g_mqtt_client.loop();
    if (g_temp_timer.tick())
      read_sensor();
  }
  else if (g_mqtt_reconnect_timer.tick() && !g_persistent_data[PFIELD::mqtt_hostname].isEmpty()) {
    if (g_mqtt_reconnect_attempts < 10) {
      g_logger.println("MQTT disconnected, attempting reconnect...");
      mqtt_connect();
      g_mqtt_reconnect_attempts++;
    }
    else {
      g_mqtt_reconnect_attempts = 0;
      WiFi.disconnect();
    }
  }*/
}

#endif