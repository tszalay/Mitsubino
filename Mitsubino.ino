#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <HeatPump.h>      // SwiCago, submodule installed in Arduino library dir
#include <ArduinoJson.h>   // ArduinoJson library v6.20.1
#include <PubSubClient.h>  // PubSubClient library v2.8

#define CONFIG_AP_NAME "Mitsubino-Config"

// global connection objects
ESP8266WebServer server(80);
WiFiClient wifi_client;
PubSubClient mqtt_client(wifi_client);
HeatPump heat_pump;

// whether we are plugged into USB or the heat pump.
// this is compile-time const because if we reconnect to the USB serial
// we may as well re-flash, ESP8266 only has one real serial port.
static const bool USE_HEATPUMP = true;

const char* heatpump_topic = "heatpump_topic/foo";
const char* heatpump_status_topic = "heatpump_topic/status";
const char* heatpump_debug_topic = "heatpump_topic/debug";
const char* heatpump_timers_topic = "heatpump_topic/timers";
const char* heatpump_set_topic = "heatpump_topic/set";
const char* heatpump_debug_set_topic = "heatpump_topic/debug_set";
bool _debugMode = false;
int SEND_ROOM_TEMP_INTERVAL_MS = 15000;
int lastTempSend = 0;


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
  LittleFS.begin();
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

// ------------- abstracted debug print helpers -------------

template<typename T>
void _debug_print_impl(String& s, const T& t) {
  if (USE_HEATPUMP)
    s.concat(t);
  else
    Serial.print(t);
}
 
template<typename T, typename... U>
void _debug_print_impl(String& s, const T& t, const U&... u) {
  _debug_print_impl(s, t);
  _debug_print_impl(s, u...);
}

template<typename... T>
void debug_print(const T&... t) {
  String s;
  if (USE_HEATPUMP)
    s.reserve(256); // long enough for most debug messages
  _debug_print_impl(s, t...);
  if (USE_HEATPUMP) {
    Serial.println("mock using heatpump, message:");
    Serial.println(s);
  }
  // else: already printed using serial.print above
}

template<typename... T>
void debug_println(const T&... t) {
  debug_print(t..., "\n");
}


// ------------- HTTP server pages -------------

// Shows webpage that displays forms to submit
void handle_persistent_forms() {
  PersistentData data;
  // use whatever we have saved to try and prepopulate the fields
  load_persistent_data(data);
  String content = F("<!DOCTYPE HTML>\r\n<html>Mitsubino Connectivity Setup <form method='get' action='save'>");
  for (int i = 0; i < data.size(); i++) {
    content += "<label>" + String(PERSISTENT_FIELD_NAMES[i]) + ": </label>";
    content += "<input name = '" + String(PERSISTENT_FIELD_NAMES[i]) + "' ";
    content += " value = '" + data[i] + "' length=64><br>";
  }
  content += "<input type='submit'></form></html>";
  server.send(200, "text/html", content);
}

// Saves the resulting POST from form submission
void handle_persistent_save() {
  PersistentData data;
  for (int i = 0; i < data.size(); i++)
    data[i] = server.arg(PERSISTENT_FIELD_NAMES[i]);
  debug_println("Received data from POST and saving to Flash:");
  print_persistent_data(data);
  save_persistent_data(data);
  server.send(200, "text/html", F("Data saved, rebooting. You may need to change networks or addresses to reconnect."));
  debug_println("Rebooting...");
  delay(1000);
  ESP.restart();
}

void blink_once() {
  digitalWrite(LED_BUILTIN, LOW);   // turn the LED on (HIGH is the voltage level)
  delay(1000);                      // wait for a second
  digitalWrite(LED_BUILTIN, HIGH);  // turn the LED off by making the voltage LOW
}

void start_server() {
  server.on("/", handle_persistent_forms);
  server.on("/save", handle_persistent_save);
  server.on("/blink", []() {
    blink_once();
    server.send(200, "text/plain", "blinking");
  });
  server.onNotFound([]() {
    digitalWrite(LED_BUILTIN, 1);
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++) { message += " " + server.argName(i) + ": " + server.arg(i) + "\n"; }
    server.send(404, "text/plain", message);
    digitalWrite(LED_BUILTIN, 0);
  });
  server.begin();
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
    server.handleClient();
    ArduinoOTA.handle();
    dnsServer.processNextRequest();
  }
  debug_println("No client or reconfiguration received, reverting to retrying wifi connection...");
  WiFi.disconnect();
}

void hpSettingsChanged() {
  const size_t bufferSize = JSON_OBJECT_SIZE(6);
  DynamicJsonDocument root(bufferSize);

  heatpumpSettings currentSettings = heat_pump.getSettings();

  root["power"] = currentSettings.power;
  root["mode"] = currentSettings.mode;
  root["temperature"] = currentSettings.temperature;
  root["fan"] = currentSettings.fan;
  root["vane"] = currentSettings.vane;
  root["wideVane"] = currentSettings.wideVane;
  //root["iSee"]        = currentSettings.iSee;

  char buffer[512];
  serializeJson(root, buffer);

  bool retain = true;
  if (!mqtt_client.publish(heatpump_topic, buffer, retain)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish to heatpump topic");
  }
}

void hpStatusChanged(heatpumpStatus currentStatus) {
  // send room temp and operating info
  const size_t bufferSizeInfo = JSON_OBJECT_SIZE(2);
  DynamicJsonDocument rootInfo(bufferSizeInfo);

  rootInfo["roomTemperature"] = currentStatus.roomTemperature;
  rootInfo["operating"] = currentStatus.operating;

  char bufferInfo[512];
  serializeJson(rootInfo, bufferInfo);

  if (!mqtt_client.publish(heatpump_status_topic, bufferInfo, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish to room temp and operation status to heatpump/status topic");
  }

  // send the timer info
  const size_t bufferSizeTimers = JSON_OBJECT_SIZE(5);
  DynamicJsonDocument rootTimers(bufferSizeTimers);

  rootTimers["mode"] = currentStatus.timers.mode;
  rootTimers["onMins"] = currentStatus.timers.onMinutesSet;
  rootTimers["onRemainMins"] = currentStatus.timers.onMinutesRemaining;
  rootTimers["offMins"] = currentStatus.timers.offMinutesSet;
  rootTimers["offRemainMins"] = currentStatus.timers.offMinutesRemaining;

  char bufferTimers[512];
  serializeJson(rootTimers, bufferTimers);

  if (!mqtt_client.publish(heatpump_timers_topic, bufferTimers, true)) {
    mqtt_client.publish(heatpump_debug_topic, "failed to publish timer info to heatpump/status topic");
  }
}

void hpPacketDebug(byte* packet, unsigned int length, char* packetDirection) {
  if (_debugMode) {
    String message;
    for (int idx = 0; idx < length; idx++) {
      if (packet[idx] < 16) {
        message += "0";  // pad single hex digits with a 0
      }
      message += String(packet[idx], HEX) + " ";
    }

    const size_t bufferSize = JSON_OBJECT_SIZE(6);
    DynamicJsonDocument root(bufferSize);

    root[packetDirection] = message;

    char buffer[512];
    serializeJson(root, buffer);

    if (!mqtt_client.publish(heatpump_debug_topic, buffer)) {
      mqtt_client.publish(heatpump_debug_topic, "failed to publish to heatpump/debug topic");
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Copy payload into message buffer
  char message[length + 1];
  for (int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';

  if (strcmp(topic, heatpump_set_topic) == 0) {  //if the incoming message is on the heatpump_set_topic topic...
    // Parse message into JSON
    const size_t bufferSize = JSON_OBJECT_SIZE(6);
    DynamicJsonDocument root(bufferSize);
    DeserializationError error = deserializeJson(root, message);

    if (error) {
      mqtt_client.publish(heatpump_debug_topic, "!root.success(): invalid JSON on heatpump_set_topic...");
      return;
    }

    // Step 3: Retrieve the values
    if (root.containsKey("power")) {
      const char* power = root["power"];
      heat_pump.setPowerSetting(power);
    }

    if (root.containsKey("mode")) {
      const char* mode = root["mode"];
      heat_pump.setModeSetting(mode);
    }

    if (root.containsKey("temperature")) {
      float temperature = root["temperature"];
      heat_pump.setTemperature(temperature);
    }

    if (root.containsKey("fan")) {
      const char* fan = root["fan"];
      heat_pump.setFanSpeed(fan);
    }

    if (root.containsKey("vane")) {
      const char* vane = root["vane"];
      heat_pump.setVaneSetting(vane);
    }

    if (root.containsKey("wideVane")) {
      const char* wideVane = root["wideVane"];
      heat_pump.setWideVaneSetting(wideVane);
    }

    if (root.containsKey("remoteTemp")) {
      float remoteTemp = root["remoteTemp"];
      heat_pump.setRemoteTemperature(remoteTemp);
    } else {
      bool result = heat_pump.update();

      if (!result) {
        mqtt_client.publish(heatpump_debug_topic, "heatpump: update() failed");
      }
    }

  } else if (strcmp(topic, heatpump_debug_set_topic) == 0) {  //if the incoming message is on the heatpump_debug_set_topic topic...
    if (strcmp(message, "on") == 0) {
      _debugMode = true;
      mqtt_client.publish(heatpump_debug_topic, "debug mode enabled");
    } else if (strcmp(message, "off") == 0) {
      _debugMode = false;
      mqtt_client.publish(heatpump_debug_topic, "debug mode disabled");
    }
  } else {  //should never get called, as that would mean something went wrong with subscribe
    mqtt_client.publish(heatpump_debug_topic, "heatpump: wrong topic received");
  }
}

void setup() {
  // Serial is either hooked up to heatpump or USB
  if (!USE_HEATPUMP) {
    Serial.begin(115200);
    Serial.println("");
  }
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  PersistentData data;
  if (!load_persistent_data(data))
    debug_println("Failed to load persistent data, still attempting to connect to WiFi");

  debug_println("Loaded persistent data:");
  print_persistent_data(data);

  // WiFi settings
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);
  WiFi.hostname(data[PFIELD::my_hostname]);
  WiFi.begin(data[PFIELD::ssid], data[PFIELD::password]);

  // OTA update settings
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(data[PFIELD::my_hostname].c_str());

  int seconds_waiting = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    seconds_waiting++;
    if (seconds_waiting > 60) {
      debug_println("Not connected in 60s, serving access point with config page");
      // spend some 10min waiting for reconfigure connection, then keep trying to connect
      start_ap_and_server();
      // revert to connection mode and keep going
      WiFi.mode(WIFI_STA);
      WiFi.begin(data[PFIELD::ssid], data[PFIELD::password]);
      break;
    }
  }
  debug_println("Connected to ", WiFi.SSID(), "\nIP address: ", WiFi.localIP().toString());

  MDNS.begin(data[PFIELD::my_hostname].c_str());
  debug_println("MDNS started");
  start_server();
  debug_println("HTTP server started");
  ArduinoOTA.begin();
  debug_println("OTA server started");

  int mqtt_port = data[PFIELD::mqtt_port].toInt();
  mqtt_client.setServer(data[PFIELD::mqtt_hostname].c_str(), mqtt_port);
  mqtt_client.setCallback(mqttCallback);
  mqtt_client.setBufferSize(1024);
  bool ret = mqtt_client.connect(
    data[PFIELD::my_hostname].c_str(),
    data[PFIELD::mqtt_username].c_str(),
    data[PFIELD::mqtt_password].c_str());
  if (ret)
    debug_println("MQTT client connected");
  else
    debug_println("MQTT client failed to connect, state: ", mqtt_client.state());

  // connect to the heatpump. Callbacks first so that the hpPacketDebug callback is available for connect()
  /*heat_pump.setSettingsChangedCallback(hpSettingsChanged);
  heat_pump.setStatusChangedCallback(hpStatusChanged);
  heat_pump.setPacketCallback(hpPacketDebug);
  heat_pump.connect(&Serial);*/
}

void loop(void) {
  if (millis() > (lastTempSend + SEND_ROOM_TEMP_INTERVAL_MS)) {  // only send the temperature every 60s
    //hpStatusChanged(heat_pump.getStatus());
    const size_t bufferSize = JSON_OBJECT_SIZE(7);
    DynamicJsonDocument root(bufferSize);

    root["power"] = 23.0f;
    root["mode"] = 5;
    root["temperature"] = 62;
    root["fan"] = 2;
    root["vane"] = 3;
    root["Data0"] = millis();
    root["Data1"] = (float)(millis() % 14537);
    //root["iSee"]        = currentSettings.iSee;

    char buffer[512];
    serializeJson(root, buffer);

    bool retain = true;
    if (!mqtt_client.publish("My_Sensors/sensor1/data", buffer, retain))
      debug_println("Failed to publish message");
    else
      debug_println("Sent MQTT packet");
    lastTempSend = millis();
  }

  server.handleClient();
  ArduinoOTA.handle();
  MDNS.update();
  if (mqtt_client.connected())
    mqtt_client.loop();
  //heat_pump.sync();
}
