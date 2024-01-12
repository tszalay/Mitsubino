// only use this code if we are compiling for the remote temp sensors

#ifdef ESP32

// Overall settings don't really matter, but to keep partition consistent:
// Use "default 4MB SPIFFS" partition if re-flashing, don't wipe
// Upload speed 921600, CPU 240MHz all seems to work

#include "MitsubinoShared.h"
#include <Adafruit_SHT4x.h> // 1.0.4

#include "esp_pm.h"
typedef esp_pm_config_esp32s2_t esp_pm_config_t;

Adafruit_SHT4x sht4{};
SimpleTimer g_temp_timer{15000};
SimpleTimer g_mqtt_reconnect_timer{100};
int g_mqtt_reconnect_attempts = 0;

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
  Serial.begin(115200);
  // connect to wifi, mDNS, OTA, MQTT, start server, etc
  WiFi.onEvent(WiFiEvent);
  configure_shared();
  Wire1.setPins(SDA1, SCL1);

  esp_pm_config_t pm_config;
  pm_config.max_freq_mhz = 240;
  pm_config.min_freq_mhz = 40;
  pm_config.light_sleep_enable = false;
  ESP_ERROR_CHECK( esp_pm_configure(&pm_config) );

  //ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_STA, 1000));
  //esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void read_sensor() {
  if (!sht4.begin(&Wire1)) {
    debug_println("Couldn't find SHT4x");
  }
  else {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    debug_println(F("SHT4x sensor connected and set to high precision"));
  }

  sensors_event_t humidity, temp;
  bool success = sht4.getEvent(&humidity, &temp);
  if (!success) {
    debug_println("Failed to read temp sensor");
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
      debug_println("Failed to publish sensor readings to reading topic");
    }
    else {
      debug_println("sent reading: ", s);
      g_reset_timer.reset();
    }
  }
  if (g_persistent_data[PFIELD::my_hostname] == "remote_temp_1")
  {
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(1));
    msg["remoteTemp"] = temp.temperature;
    String s;
    serializeJson(msg, s);
    debug_println("sending control setting: ", s);
    if (!g_mqtt_client.publish("heatpumps/hp_livingroom/control", s.c_str(), false))
      debug_println("Failed to publish sensor readings to living room control topic");
    if (!g_mqtt_client.publish("heatpumps/hp_kitchen/control", s.c_str(), false))
      debug_println("Failed to publish sensor readings to kitchen control topic");
  }

}

void loop() {
  loop_shared();

  // crappy ad-hoc disconnect state machine. ESP8266 is much better at this.
  if (WiFi.status() != WL_CONNECTED) {
    debug_println("WiFi disconnected, attempting reconnect");
    WiFi.disconnect();
    WiFi.reconnect();
    debug_println("Reconnect attempt complete, current status ", WiFi.status());
    for (int i=0; i<10; i++) {
      if (WiFi.status() == WL_CONNECTED)
        break;
      delay(1000);
    }
    return;
  }

  if (g_mqtt_client.connected()) {
    g_mqtt_reconnect_attempts = 0;
    g_mqtt_client.loop();
    if (g_temp_timer.tick())
      read_sensor();
  }
  else if (g_mqtt_reconnect_timer.tick() && !g_persistent_data[PFIELD::mqtt_hostname].isEmpty()) {
    if (g_mqtt_reconnect_attempts < 10) {
      debug_println("MQTT disconnected, attempting reconnect...");
      mqtt_connect();
      g_mqtt_reconnect_attempts++;
    }
    else {
      g_mqtt_reconnect_attempts = 0;
      WiFi.disconnect();
    }
  }
}

#endif