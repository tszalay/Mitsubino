// only use this code if we are compiling for the remote temp sensors

#ifdef ESP32

#include "MitsubinoShared.h"
#include <Adafruit_SHT4x.h>

Adafruit_SHT4x sht4{};
SimpleTimer g_temp_timer{15000};
SimpleTimer g_mqtt_reconnect_timer{5000};

void handle_mqtt_message(char* topic, byte* payload, unsigned int length) {
  /*if (String(topic) != get_topic_name("control")) {
    debug_println("Received message at unrecognized topic: ", topic);
    return;
  }*/

  // this is probably fine
  String message;
  message.concat((const char*)payload, length);
  debug_println("Got MQTT message on topic ", topic, ": ", message);
  /*DynamicJsonDocument root(JSON_OBJECT_SIZE(6));
  DeserializationError error = deserializeJson(root, message.c_str());

  if (error) {
    debug_println("Error parsing received mqtt message: ", error.c_str());
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
  debug_println("Updated heat pump");*/
}

void setup() {
  // connect to wifi, mDNS, OTA, MQTT, start server, etc
  WiFi.onEvent(WiFiEvent);
  configure_shared();
  Wire1.setPins(SDA1, SCL1);
  if (!sht4.begin(&Wire1)) {
    debug_println("Couldn't find SHT4x");
  }
  else {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    debug_println(F("SHT4x sensor connected and set to high precision"));
  }
}

void read_sensor() {
  sensors_event_t humidity, temp;
  if (!sht4.getEvent(&humidity, &temp)) {
    debug_println("Failed to read temp sensor");
    return;
  }
  DynamicJsonDocument msg(JSON_OBJECT_SIZE(2));
  msg["temperature"] = temp.temperature;
  msg["humidity"] = humidity.relative_humidity;

  String s;
  serializeJson(msg, s);

  // don't retain these readings, so that the heat pump unit can fallback to
  // internal thermostat if they stop sending for some reason
  if (!g_mqtt_client.publish(get_topic_name("reading").c_str(), s.c_str(), false))
    debug_println("Failed to publish sensor readings to reading topic");
  else
    debug_println("sent reading: ", s);
}

void loop() {
  loop_shared();
  if (!WiFi.isConnected()) {
    delay(1000);
    return;
  }

  if (g_mqtt_client.connected()) {
    g_mqtt_client.loop();
    if (g_temp_timer.tick())
      read_sensor();
  }
  else if (g_mqtt_reconnect_timer.tick()) {
    debug_println("MQTT disconnected, attempting reconnect...");
    mqtt_connect();
  }
}

#endif