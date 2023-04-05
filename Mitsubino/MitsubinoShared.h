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
#endif

#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <ArduinoJson.h>   // ArduinoJson library v6.20.1
#include <PubSubClient.h>  // PubSubClient library v2.8

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
  void reset() {
    last_tick = millis();
  }
};

// helps us reset if we haven't sent anything in a while, since
// ESP WDTs aren't doing it for us
SimpleTimer g_reset_timer{120*1000};

// ------------- abstracted debug print helpers -------------

// global where we are storing the debug log
String g_debug_log_buffer;
const size_t DEBUG_LOG_BUFFER_SIZE = 1024;

void clear_debug_log() {
  g_debug_log_buffer.remove(0, g_debug_log_buffer.length());
}

template<typename T>
void _debug_print_impl(const T& t) {
  String s(t);
  if (s.length() + g_debug_log_buffer.length() >= (DEBUG_LOG_BUFFER_SIZE-1)) {
    clear_debug_log();
    g_debug_log_buffer.concat("-- truncated --\n");
  }
  g_debug_log_buffer.concat(s);
}

template<typename T, typename... U>
void _debug_print_impl(const T& t, const U&... u) {
  _debug_print_impl(t);
  _debug_print_impl(u...);
}

template<typename... T>
void debug_print(const T&... t) {
  _debug_print_impl(t...);
}

template<typename... T>
void debug_println(const T&... t) {
  debug_print(millis(), ": ", t..., "\n");
}

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
      debug_println("File ", name, " could not be read");
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
      debug_println("File ", name, " could not be opened for write");
      return false;
    }
    f.print(data[i]);
  }
  LittleFS.end();
  return true;
}

void print_persistent_data(PersistentData& data) {
  for (int i = 0; i < data.size(); i++)
    debug_println(PERSISTENT_FIELD_NAMES[i], " = ", data[i]);
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
ESP8266/32 Mitsubino Server version 1.0.0:<br>
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
  debug_println("Received data from POST and saving to Flash:");
  print_persistent_data(data);
  save_persistent_data(data);
  g_server.send(200, "text/html", F("Data saved, rebooting. You may need to change networks or addresses to reconnect."));
  debug_println("Rebooting...");
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
    g_server.send(200, "text/plain", g_debug_log_buffer);
    clear_debug_log();
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

// Set up an access point with SSID Mitsubino and PW Mitsubino
void start_ap_and_server() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.softAP(CONFIG_AP_NAME, "");
  start_server();
  debug_println(CONFIG_AP_NAME, " wifi network started");
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
  debug_println("No client or reconfiguration received, reverting to retrying wifi connection...");
  WiFi.disconnect();
}

void mqtt_connect() {
  bool ret = g_mqtt_client.connect(
    g_persistent_data[PFIELD::my_hostname].c_str(),
    g_persistent_data[PFIELD::mqtt_username].c_str(),
    g_persistent_data[PFIELD::mqtt_password].c_str()
  );

  if (ret) {
    debug_println("MQTT client connected");
    g_mqtt_client.subscribe(get_topic_name("control").c_str());
  }
  else {
    debug_println("MQTT client failed to connect, state: ", g_mqtt_client.state());
  }
}

// ESP32-specific debug handler
#ifdef ESP32
void WiFiEvent(WiFiEvent_t event);
#endif

void configure_shared() {
  g_debug_log_buffer.reserve(DEBUG_LOG_BUFFER_SIZE);
  if (!load_persistent_data(g_persistent_data))
    debug_println("Failed to fully load persistent data, still attempting to connect to WiFi:");
  else
    debug_println("Loaded persistent data:");
  print_persistent_data(g_persistent_data);

  // WiFi settings
#ifdef ESP32
  WiFi.onEvent(WiFiEvent);
#endif
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
      debug_println(F("Not connected in 60s or SSID is blank, serving access point with config page"));
      // spend some 10min waiting for reconfigure connection, then keep trying to connect
      start_ap_and_server();
      // revert to connection mode and keep going
      WiFi.mode(WIFI_STA);
      WiFi.begin(g_persistent_data[PFIELD::ssid].begin(), g_persistent_data[PFIELD::password].begin());
      break;
    }
  }
  debug_println("Connected to ", WiFi.SSID(), "\nIP address: ", WiFi.localIP().toString());

  MDNS.begin(g_persistent_data[PFIELD::my_hostname].c_str());
  debug_println("MDNS started");
  start_server();
  debug_println("HTTP server started");
  ArduinoOTA.begin();
  debug_println("OTA server started");

  int mqtt_port = g_persistent_data[PFIELD::mqtt_port].toInt();
  g_mqtt_client.setServer(g_persistent_data[PFIELD::mqtt_hostname].c_str(), mqtt_port);
  g_mqtt_client.setCallback(handle_mqtt_message);
  g_mqtt_client.setBufferSize(1024);
  mqtt_connect();

  g_reset_timer.reset();
}

void loop_shared() {
  // if we haven't sent anything in a long time, just do a reset
  if (g_reset_timer.tick())
    ESP.restart();    
  g_server.handleClient();
  ArduinoOTA.handle();
#ifdef ESP8266
  MDNS.update();
#endif
  // this gives other background tasks a chance to run on ESP platforms
  // and nothing we do really needs to happen that fast
  delay(50);
}

#ifdef ESP32
void WiFiEvent(WiFiEvent_t event)
{
    debug_println("[WiFi-event] event: ", event);

    switch (event) {
        case ARDUINO_EVENT_WIFI_READY:
            debug_println("WiFi interface ready");
            break;
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            debug_println("Completed scan for access points");
            break;
        case ARDUINO_EVENT_WIFI_STA_START:
            debug_println("WiFi client started");
            break;
        case ARDUINO_EVENT_WIFI_STA_STOP:
            debug_println("WiFi clients stopped");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            debug_println("Connected to access point");
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            debug_println("Disconnected from WiFi access point");
            break;
        case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
            debug_println("Authentication mode of access point has changed");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            debug_println("Obtained IP address: ", WiFi.localIP());
            break;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            debug_println("Lost IP address and IP address is reset to 0");
            break;
        case ARDUINO_EVENT_WPS_ER_SUCCESS:
            debug_println("WiFi Protected Setup (WPS): succeeded in enrollee mode");
            break;
        case ARDUINO_EVENT_WPS_ER_FAILED:
            debug_println("WiFi Protected Setup (WPS): failed in enrollee mode");
            break;
        case ARDUINO_EVENT_WPS_ER_TIMEOUT:
            debug_println("WiFi Protected Setup (WPS): timeout in enrollee mode");
            break;
        case ARDUINO_EVENT_WPS_ER_PIN:
            debug_println("WiFi Protected Setup (WPS): pin code in enrollee mode");
            break;
        case ARDUINO_EVENT_WIFI_AP_START:
            debug_println("WiFi access point started");
            break;
        case ARDUINO_EVENT_WIFI_AP_STOP:
            debug_println("WiFi access point  stopped");
            break;
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            debug_println("Client connected");
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            debug_println("Client disconnected");
            break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
            debug_println("Assigned IP address to client");
            break;
        case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
            debug_println("Received probe request");
            break;
        case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
            debug_println("AP IPv6 is preferred");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
            debug_println("STA IPv6 is preferred");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP6:
            debug_println("Ethernet IPv6 is preferred");
            break;
        case ARDUINO_EVENT_ETH_START:
            debug_println("Ethernet started");
            break;
        case ARDUINO_EVENT_ETH_STOP:
            debug_println("Ethernet stopped");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            debug_println("Ethernet connected");
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            debug_println("Ethernet disconnected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            debug_println("Obtained IP address");
            break;
        default: break;
    }
}
#endif