// This file contains functionality shared between the actual heatpump controllers
// which run on ESP8266, and the remote temp widgets which run on ESP32s.
// I apologize for abusing the chip in order to change behavior, but Arduino
// makes it borderline impossible to share behavior between different sketches,
// and this is a simple workaround.

#pragma once

#ifdef ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#else
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include "ESP32_NOW.h"
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros
#endif

#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>   // ArduinoJson library v6.21.4
#include <PubSubClient.h>  // PubSubClient library v2.8.0

#include "States.h"
#include "Logger.h"

#define CONFIG_AP_NAME "Mitsubino-Config"

// topics are HP_TOPIC_BASE/HOSTNAME/{status,settings,timers,control}
#define HP_TOPIC_BASE "heatpumps/"

// shared defines that apply to both code units
void handle_mqtt_message(char* topic, byte* payload, unsigned int length);

// global connection objects
#ifdef ESP8266
ESP8266WebServer g_server(80);
#else
WebServer g_server(80);
#endif

WiFiClient g_wifi_client;
PubSubClient g_mqtt_client(g_wifi_client);

// timer intervals
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

// helps us reset if we haven't sent anything in a while, since
// ESP WDTs aren't doing it for us
SimpleTimer g_reset_timer{120*1000};

Logger g_logger{2048};

// ------------- persistent data save/restore helpers -------------

// Change this to add fields, it will automatically get propagated to config page etc.
#define FOR_ALL_FIELDS(FUN) \
  FUN(ssid) \
  FUN(password) \
  FUN(my_hostname) \
  FUN(mqtt_hostname) \
  FUN(mqtt_port) \
  FUN(mqtt_username) \
  FUN(mqtt_password)

#define STRING_HELPER(X) #X,
#define ENUM_HELPER(X) X,

namespace PFIELD {
enum _FIELD : size_t {
  FOR_ALL_FIELDS(ENUM_HELPER)
    SIZE
};
}

using PersistentData = std::array<String, PFIELD::SIZE>;

PersistentData g_persistent_data;

static constexpr std::array<const char*, PFIELD::SIZE> PERSISTENT_FIELD_NAMES = {
  FOR_ALL_FIELDS(STRING_HELPER)
};

bool load_persistent_data(PersistentData& data) {
  LittleFS.begin();
  for (int i = 0; i < data.size(); i++) {
    String name = String(PERSISTENT_FIELD_NAMES[i]);
    File f = LittleFS.open("/" + name, "r");
    if (!f) {
      g_logger.println("File ", name, " could not be read");
      return false;
    }
    data[i] = f.readString();
  }
  LittleFS.end();
  return true;
}

bool save_persistent_data(PersistentData& data) {
#ifdef ESP8266
  LittleFS.begin();
#else
  // format if necessary here
  LittleFS.begin(true);
#endif
  for (int i = 0; i < data.size(); i++) {
    String name = String(PERSISTENT_FIELD_NAMES[i]);
    File f = LittleFS.open("/" + name, "w");
    if (!f) {
      g_logger.println("File ", name, " could not be opened for write");
      return false;
    }
    f.print(data[i]);
  }
  LittleFS.end();
  return true;
}

void print_persistent_data(PersistentData& data) {
  for (int i = 0; i < data.size(); i++)
    g_logger.println(PERSISTENT_FIELD_NAMES[i], " = ", data[i]);
}

String get_topic_name(const char* subtopic) {
  String topic;
  topic.reserve(64);
  topic += HP_TOPIC_BASE;
  const String& hostname = g_persistent_data[PFIELD::my_hostname];
  topic += hostname.isEmpty() ? String("hp_default") : hostname;
  topic += "/";
  topic += subtopic;
  return topic;
}


// ------------- HTTP server pages -------------

/* Root page */
const char ROOT_PAGE_BODY[] PROGMEM = R"=====(
<!DOCTYPE html><html><body><p>
ESP8266/32 Mitsubino Server version 1.1.0:<br>
<a href="config">Configuration</a><br>
<a href="log">View log</a><br>
<a href="restart">Restart</a><br>
<a href="blink">Blink LED</a>
</p></body></html>
)=====";

/* Auto-refreshing webpage that fetches latest debug log every 2s */
const char LOG_PAGE_BODY[] PROGMEM = R"=====(
<!DOCTYPE html><html>
<div style="white-space: pre-line"><p>
ESP debug log:<br>
<span id="log_text"><br></span>
</p></div>
<script>
setInterval(getData, 2000);
function getData() {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById("log_text").innerHTML += this.responseText;
    }
  };
  xhttp.open("GET", "get_log", true);
  xhttp.send();
}
</script>
</body></html>
)=====";


// Shows webpage that displays forms to submit
void handle_persistent_forms() {
  // use whatever we have saved to try and prepopulate the fields
  String content = F("<!DOCTYPE HTML>\r\n<html>Mitsubino Connectivity Setup <form method='get' action='save'>");
  for (int i = 0; i < g_persistent_data.size(); i++) {
    content += "<label>" + String(PERSISTENT_FIELD_NAMES[i]) + ": </label>";
    content += "<input name = '" + String(PERSISTENT_FIELD_NAMES[i]) + "' ";
    content += " value = '" + g_persistent_data[i] + "' length=64><br>";
  }
  content += "<input type='submit'></form></html>";
  g_server.send(200, "text/html", content);
}

// Saves the resulting POST from form submission
void handle_persistent_save() {
  PersistentData data;
  for (int i = 0; i < data.size(); i++)
    data[i] = g_server.arg(PERSISTENT_FIELD_NAMES[i]);
  g_logger.println("Received data from POST and saving to Flash:");
  print_persistent_data(data);
  save_persistent_data(data);
  g_server.send(200, "text/html", F("Data saved, rebooting. You may need to change networks or addresses to reconnect."));
  g_logger.println("Rebooting...");
  delay(1000);
  ESP.restart();
}

#ifdef ESP8266
void blink_once() {
  digitalWrite(LED_BUILTIN, LOW);   // turn the LED on (HIGH is the voltage level)
  delay(1000);                      // wait for a second
  digitalWrite(LED_BUILTIN, HIGH);  // turn the LED off by making the voltage LOW
}
#endif

void start_server() {
  g_server.on("/", [] () { g_server.send(200, "text/html", String(ROOT_PAGE_BODY)); });
  g_server.on("/config", handle_persistent_forms);
  g_server.on("/save", handle_persistent_save);
  g_server.on("/log", [] () { g_server.send(200, "text/html", String(LOG_PAGE_BODY)); });
  g_server.on("/get_log", [] () {
    g_server.send(200, "text/plain", g_logger.get());
    g_logger.clear();
  });
  g_server.on("/restart", [] () { g_server.send(200, "text/plain", "Restarting..."); ESP.restart(); });
#ifdef ESP8266
  g_server.on("/blink", []() { blink_once(); g_server.send(200, "text/plain", "blinking"); });
#endif
  g_server.onNotFound([]() {
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += g_server.uri();
    message += "\nMethod: ";
    message += (g_server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += g_server.args();
    message += "\n";
    for (uint8_t i = 0; i < g_server.args(); i++) { message += " " + g_server.argName(i) + ": " + g_server.arg(i) + "\n"; }
    g_server.send(404, "text/plain", message);
  });
  g_server.begin();
}

// ------------- end HTTP server code -------------

// ------------- begin ESPNOW code -------------

#ifndef ESP8266

const uint8_t ESP_NOW_BROADCAST_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void OnDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
  g_logger.println("Packet send status: ", (status == ESP_NOW_SEND_SUCCESS) ? "success" : "failure");
}

void OnDataRecv(const esp_now_recv_info_t *rx_info, const uint8_t *incomingData, int len) {
  g_logger.println("Packet received from MAC: ", mac2str(rx_info->src_addr));
  g_logger.println("Data received: ", String((char*)incomingData, len));
}
#endif

// ------------- end ESPNOW code -------------


// Set up an access point with SSID Mitsubino and PW Mitsubino
void start_ap_and_server() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.softAP(CONFIG_AP_NAME, "");
  start_server();
  g_logger.println(CONFIG_AP_NAME, " wifi network started");
  DNSServer dnsServer;
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  // just in case I need to reflash while in disconnected mode
  ArduinoOTA.begin();
  auto start_millis = millis();
  // only serve the access point for 10 minutes, then go into suspend
  auto end_millis = start_millis + 10 * 60 * 1000;
  while (millis() < end_millis) {
    g_server.handleClient();
    ArduinoOTA.handle();
    dnsServer.processNextRequest();
  }
  g_logger.println("No client or reconfiguration received, reverting to retrying wifi connection...");
  WiFi.disconnect();
}

void mqtt_connect() {
  bool ret = g_mqtt_client.connect(
    g_persistent_data[PFIELD::my_hostname].c_str(),
    g_persistent_data[PFIELD::mqtt_username].c_str(),
    g_persistent_data[PFIELD::mqtt_password].c_str()
  );

  if (ret) {
    g_logger.println("MQTT client connected");
    g_mqtt_client.subscribe(get_topic_name("control").c_str());
  }
  else {
    g_logger.println("MQTT client failed to connect, state: ", g_mqtt_client.state());
  }
}

void configure_shared() {
  if (!load_persistent_data(g_persistent_data))
    g_logger.println("Failed to fully load persistent data, still attempting to connect to WiFi:");
  else
    g_logger.println("Loaded persistent data:");
  print_persistent_data(g_persistent_data);

  // WiFi settings
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
#ifdef ESP8266
  // needed for compatibility with certain (my) ASUS routers
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);
#else
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G);
#endif
  WiFi.hostname(g_persistent_data[PFIELD::my_hostname]);
  WiFi.begin(g_persistent_data[PFIELD::ssid].c_str(), g_persistent_data[PFIELD::password].c_str());

  // OTA update settings
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(g_persistent_data[PFIELD::my_hostname].c_str());

  int wait_start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    // give wifi 10 minutes to boot up in case of power failure; don't wait on first boot with blank info
    if (millis()-wait_start > 10*60*1000 || g_persistent_data[PFIELD::ssid].isEmpty()) {
      g_logger.println(F("Not connected in 60s or SSID is blank, serving access point with config page"));
      // spend some 10min waiting for reconfigure connection, then keep trying to connect
      start_ap_and_server();
      // revert to connection mode and keep going
      WiFi.mode(WIFI_STA);
      WiFi.begin(g_persistent_data[PFIELD::ssid].begin(), g_persistent_data[PFIELD::password].begin());
      break;
    }
  }
  g_logger.println("Connected to ", WiFi.SSID());
  g_logger.println("IP address: ", WiFi.localIP().toString());

  MDNS.begin(g_persistent_data[PFIELD::my_hostname].c_str());
  g_logger.println("MDNS started");
  start_server();
  g_logger.println("HTTP server started");
  ArduinoOTA.begin();
  g_logger.println("OTA server started");

  int mqtt_port = g_persistent_data[PFIELD::mqtt_port].toInt();
  g_mqtt_client.setServer(g_persistent_data[PFIELD::mqtt_hostname].c_str(), mqtt_port);
  g_mqtt_client.setCallback(handle_mqtt_message);
  g_mqtt_client.setBufferSize(1024);
  mqtt_connect();

#ifndef ESP8266
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
    if (esp_now_add_peer(&peerInfo) != ESP_OK){
      g_logger.println("Failed to add peer");
    }
    else {
      g_logger.println("Successfully added peer");
    }
  }

  uint32_t esp_now_version = 1;
  auto err = esp_now_get_version(&esp_now_version);
  if (err != ESP_OK) {
    esp_now_version = 1;
  }
  const uint32_t max_data_len = (esp_now_version == 1) ? ESP_NOW_MAX_DATA_LEN : ESP_NOW_MAX_DATA_LEN_V2;
  g_logger.println("ESP-NOW version: ", esp_now_version, ", max data length: ", max_data_len);
#endif

  g_reset_timer.reset();
}

SimpleTimer g_espnow_timer{5000};

void loop_shared() {
  // if we haven't sent anything in a long time, just do a reset
  if (g_reset_timer.tick()) {
    g_logger.println("Resetting due to g_reset_timer trip");
    delay(5000);
    ESP.restart();
  }
  if (g_espnow_timer.tick()) {
    g_logger.println("Sending ESPNOW message");
    String msg = "Hello from ";
    msg += g_persistent_data[PFIELD::my_hostname];
    msg += " at time " + String(millis());
    if (esp_now_send(ESP_NOW_BROADCAST_MAC, (const uint8_t*)msg.c_str(), msg.length()+1) != ESP_OK) {
      g_logger.println("esp_now_send failed!");
    }
  }
  g_server.handleClient();
  ArduinoOTA.handle();
#ifdef ESP8266
  MDNS.update();
#endif
  // this gives other background tasks a chance to run on ESP platforms
  // and nothing we do really needs to happen that fast
  delay(50);
}
