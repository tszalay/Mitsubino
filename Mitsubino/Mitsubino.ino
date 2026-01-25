#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include "ESP32_NOW.h"
#include <esp_now.h>
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros

#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ArduinoJson.h>   // ArduinoJson library v6.21.4
#include <PubSubClient.h>  // PubSubClient library v2.8.0
#include <Adafruit_SHT4x.h> // 1.0.4

#include "Logger.h"
#include "PersistentData.h"
#include "WifiStates.h"
#include "MQTTClient.h"
#include "HTTPConfigServer.h"

#ifdef ESP32

// Overall settings don't really matter, but to keep partition consistent:
// Use "default 4MB SPIFFS" partition if re-flashing, don't wipe
// Upload speed 921600, CPU 240MHz all seems to work


Logger g_logger{2048};
PersistentData g_persistent_data(&g_logger);

WifiClientStateMachine* g_wifi = nullptr;
MQTTStateMachine* g_mqtt = nullptr;
HTTPConfigServer* g_server = nullptr;

Adafruit_SHT4x sht4{};
SimpleTimer g_temp_timer{15000};

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


const uint8_t ESP_NOW_BROADCAST_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// we are using manual encryption because broadcast mode doesn't support encryption
static const char* ESP_NOW_MANUAL_KEY = "Bite my shiny metal ass";

void OnDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
  g_logger.println("Packet send status: ", (status == ESP_NOW_SEND_SUCCESS) ? "success" : "failure");
}

void OnDataRecv(const esp_now_recv_info_t *rx_info, const uint8_t *incomingData, int len) {
  String msg((const char*)incomingData, len);
  esp_now_manual_xor(msg);
  g_logger.println("Packet received from MAC: ", mac2str(rx_info->src_addr));
  g_logger.println("Data received: ", msg);
}

void esp_now_manual_xor(String& msg) {
  const char* k = ESP_NOW_MANUAL_KEY;
  for (char& c : msg) {
    c ^= *k;
    if (++k == 0)
      k = ESP_NOW_MANUAL_KEY;
  }
}


void setup() {
  Serial.begin(115200);
  g_logger.set_serial(true);  

  if (!g_persistent_data.load())
    g_logger.println("Failed to fully load persistent data, still attempting to connect to WiFi:");
  else
    g_logger.println("Loaded persistent data:");
  g_persistent_data.print();

  g_wifi = new WifiClientStateMachine(&g_logger, g_persistent_data.my_hostname, g_persistent_data.ssid, g_persistent_data.password);

  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(g_persistent_data.my_hostname.c_str());

  MDNS.begin(g_persistent_data.my_hostname.c_str());
  g_server = new HTTPConfigServer(&g_logger, &g_persistent_data);
  ArduinoOTA.begin();

  // Initialize the ESP-NOW protocol
  if (esp_now_init() != ESP_OK) {
    g_logger.println("ESP-NOW failed to init");
    //ESP.restart();
  }
  else {
    g_logger.println("Initialized ESP-NOW");
    uint8_t baseMac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, baseMac) == ESP_OK) {
      g_logger.println("WiFi MAC address: ", mac2str(baseMac));
    }
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, ESP_NOW_BROADCAST_MAC, 6);
    peerInfo.channel = 0; // Use channel 0 for default
    peerInfo.encrypt = false;
    auto ret = esp_now_add_peer(&peerInfo);
    if (ret == ESP_OK)
      g_logger.println("Successfully added peer");
    else
      g_logger.println("Failed to add peer: ", ret);
  }


  uint32_t esp_now_version = 1;
  auto err = esp_now_get_version(&esp_now_version);
  if (err != ESP_OK) {
    esp_now_version = 1;
  }
  const uint32_t max_data_len = (esp_now_version == 1) ? ESP_NOW_MAX_DATA_LEN : ESP_NOW_MAX_DATA_LEN_V2;
  g_logger.println("ESP-NOW version: ", esp_now_version, ", max data length: ", max_data_len);

  WIRE_TO_USE.setPins(SDA_TO_USE, SCL_TO_USE);
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
    //if (!g_mqtt_client.publish(get_topic_name("reading").c_str(), s.c_str(), false)) {
    //  g_logger.println("Failed to publish sensor readings to reading topic");
    //}
    //else {
    //  g_logger.println("sent reading: ", s);
    //}
  }
  if (g_persistent_data.my_hostname == "remote_temp_1")
  {
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(1));
    msg["remoteTemp"] = temp.temperature;
    String s;
    serializeJson(msg, s);
    g_logger.println("sending control setting: ", s);
    //if (!g_mqtt_client.publish("heatpumps/hp_livingroom/control", s.c_str(), false))
    //  g_logger.println("Failed to publish sensor readings to living room control topic");
    //if (!g_mqtt_client.publish("heatpumps/hp_kitchen/control", s.c_str(), false))
    //  g_logger.println("Failed to publish sensor readings to kitchen control topic");
  }

}

SimpleTimer g_espnow_timer{5000};

void loop() {
  if (g_espnow_timer.tick()) {
    g_logger.println("Sending ESPNOW message");
    String msg = "Hello from ";
    msg += g_persistent_data.my_hostname;
    msg += " at time " + String(millis());
    esp_now_manual_xor(msg);
    if (esp_now_send(ESP_NOW_BROADCAST_MAC, (const uint8_t*)msg.c_str(), msg.length()+1) != ESP_OK) {
      g_logger.println("esp_now_send failed!");
    }
  }
  g_wifi->loop();
  g_server->loop();
  ArduinoOTA.handle();
}

#endif