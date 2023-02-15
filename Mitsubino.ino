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

// whether we are plugged into USB or the heat pump.
// this is compile-time const because if we reconnect to the USB serial
// we may as well re-flash, ESP8266 only has one real serial port.
static const bool USE_HEATPUMP = true;

// topics are HP_TOPIC_BASE/HOSTNAME/{status,settings,timers,control}
#define HP_TOPIC_BASE "heatpumps/"

// global connection objects
ESP8266WebServer g_server(80);
WiFiClient g_wifi_client;
PubSubClient g_mqtt_client(g_wifi_client);
HeatPump g_heat_pump;

// global where we are storing the debug log
String g_debug_log_buffer;
const size_t DEBUG_LOG_BUFFER_SIZE = 1024;

// timer intervals
struct SimpleTimer {
  const int interval;
  int next_tick{0};
  bool tick() {
    int time = millis();
    if (time >= next_tick) {
      next_tick = time + interval;
      return true;
    }
    return false;
  }
  void reset() {
    next_tick = millis() + interval;
  }
};

SimpleTimer g_sync_timer{100};
SimpleTimer g_send_timer{15000};

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

// ------------- abstracted debug print helpers -------------

void clear_debug_log() {
  g_debug_log_buffer.remove(0, g_debug_log_buffer.length());
}

template<typename T>
void _debug_print_impl(const T& t) {
  String s(t);
  if (s.length() + g_debug_log_buffer.length() >= (DEBUG_LOG_BUFFER_SIZE-1))
    clear_debug_log();
  g_debug_log_buffer.concat(s);
  if (!USE_HEATPUMP)
    Serial.print(t);
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
  debug_print(t..., "\n");
}


// ------------- HTTP server pages -------------

/* Root page */
const char ROOT_PAGE_BODY[] PROGMEM = R"=====(
<!DOCTYPE html><html><body><p>
ESP8266 Mitsubino Server:<br>
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
ESP8266 debug log:<br>
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

void blink_once() {
  digitalWrite(LED_BUILTIN, LOW);   // turn the LED on (HIGH is the voltage level)
  delay(1000);                      // wait for a second
  digitalWrite(LED_BUILTIN, HIGH);  // turn the LED off by making the voltage LOW
}

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
  g_server.on("/blink", []() { blink_once(); g_server.send(200, "text/plain", "blinking"); });

  g_server.onNotFound([]() {
    digitalWrite(LED_BUILTIN, 1);
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
    digitalWrite(LED_BUILTIN, 0);
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

void send_hp_settings(const heatpumpSettings& settings) {
  DynamicJsonDocument msg(JSON_OBJECT_SIZE(7));
  msg["power"] = settings.power;
  msg["mode"] = settings.mode;
  msg["temperature"] = settings.temperature;
  msg["fan"] = settings.fan;
  msg["vane"] = settings.vane;
  msg["wideVane"] = settings.wideVane;
  msg["connected"] = settings.connected;

  String s;
  serializeJson(msg, s);

  if (!g_mqtt_client.publish(get_topic_name("settings").c_str(), s.c_str(), true))
    debug_print("Failed to publish settings change to settings topic");
  else
    debug_println("sent settings: ", s);
}

void send_hp_status(const heatpumpStatus& status) {
  {
    // send room temp and operating info
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(3));
    msg["roomTemperature"] = status.roomTemperature;
    msg["operating"] = status.operating;
    msg["compressorFrequency"] = status.compressorFrequency;

    String s;
    serializeJson(msg, s);
    if (!g_mqtt_client.publish(get_topic_name("status").c_str(), s.c_str(), true))
      debug_println("failed to publish to room temp and operation status to status topic");
    else
      debug_println("sent status: ", s);
  }

  {
    // send the timer info
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(5));

    msg["mode"] = status.timers.mode;
    msg["onMins"] = status.timers.onMinutesSet;
    msg["onRemainMins"] = status.timers.onMinutesRemaining;
    msg["offMins"] = status.timers.offMinutesSet;
    msg["offRemainMins"] = status.timers.offMinutesRemaining;

    String s;
    serializeJson(msg, s);

    if (!g_mqtt_client.publish(get_topic_name("timers").c_str(), s.c_str(), true))
      debug_println("failed to publish timer info to timer topic");
  }

  g_send_timer.reset();
}

void hp_debug(byte* packet, unsigned int length, char* packetDirection) {
  debug_println("Heatpump packet of length ", length, " with direction ", packetDirection);
  for (int idx = 0; idx < length; idx++) {
    if (packet[idx] < 16)
      debug_print("0"); // pad single hex digits with a 0
    debug_print(String(packet[idx], HEX), " ");
  }
  debug_print("\n");
}

void mock_heat_pump_sync() {
  if (!g_send_timer.tick())
    return;
  heatpumpStatus status;
  heatpumpSettings settings;
  status.compressorFrequency = millis() % 150;
  status.roomTemperature = (float)(millis() % 55);
  status.operating = true;
  status.timers.mode = "FOOBAR";
  settings.mode = "AUTOBOT";
  settings.connected = true;
  settings.fan = "fan";
  settings.wideVane = "wv";
  settings.vane = millis() % 3 ? "low" : "high";
  settings.temperature = 22.3f + (millis() % 10);
  settings.power = "on";
  send_hp_status(status);
  send_hp_settings(settings);
}

/*void mqttCallback(char* topic, byte* payload, unsigned int length) {
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
      g_mqtt_client.publish(heatpump_debug_topic, "!root.success(): invalid JSON on heatpump_set_topic...");
      return;
    }

    // Step 3: Retrieve the values
    if (root.containsKey("power")) {
      const char* power = root["power"];
      g_heat_pump.setPowerSetting(power);
    }

    if (root.containsKey("mode")) {
      const char* mode = root["mode"];
      g_heat_pump.setModeSetting(mode);
    }

    if (root.containsKey("temperature")) {
      float temperature = root["temperature"];
      g_heat_pump.setTemperature(temperature);
    }

    if (root.containsKey("fan")) {
      const char* fan = root["fan"];
      g_heat_pump.setFanSpeed(fan);
    }

    if (root.containsKey("vane")) {
      const char* vane = root["vane"];
      g_heat_pump.setVaneSetting(vane);
    }

    if (root.containsKey("wideVane")) {
      const char* wideVane = root["wideVane"];
      g_heat_pump.setWideVaneSetting(wideVane);
    }

    if (root.containsKey("remoteTemp")) {
      float remoteTemp = root["remoteTemp"];
      g_heat_pump.setRemoteTemperature(remoteTemp);
    } else {
      bool result = g_heat_pump.update();

      if (!result) {
        g_mqtt_client.publish(heatpump_debug_topic, "heatpump: update() failed");
      }
    }

  } else if (strcmp(topic, heatpump_debug_set_topic) == 0) {  //if the incoming message is on the heatpump_debug_set_topic topic...
    if (strcmp(message, "on") == 0) {
      _debugMode = true;
      g_mqtt_client.publish(heatpump_debug_topic, "debug mode enabled");
    } else if (strcmp(message, "off") == 0) {
      _debugMode = false;
      g_mqtt_client.publish(heatpump_debug_topic, "debug mode disabled");
    }
  } else {  //should never get called, as that would mean something went wrong with subscribe
    g_mqtt_client.publish(heatpump_debug_topic, "heatpump: wrong topic received");
  }
}*/

void setup() {
  g_debug_log_buffer.reserve(DEBUG_LOG_BUFFER_SIZE);
  // Serial is either hooked up to heatpump or USB
  if (!USE_HEATPUMP) {
    Serial.begin(115200);
    Serial.println("");
  }
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  if (!load_persistent_data(g_persistent_data))
    debug_println("Failed to fully load persistent data, still attempting to connect to WiFi:");
  else
    debug_println("Loaded persistent data:");
  print_persistent_data(g_persistent_data);

  // WiFi settings
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);
  WiFi.hostname(g_persistent_data[PFIELD::my_hostname]);
  WiFi.begin(g_persistent_data[PFIELD::ssid], g_persistent_data[PFIELD::password]);

  // OTA update settings
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(g_persistent_data[PFIELD::my_hostname].c_str());

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
      WiFi.begin(g_persistent_data[PFIELD::ssid], g_persistent_data[PFIELD::password]);
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
  //g_mqtt_client.setCallback(mqttCallback);
  g_mqtt_client.setBufferSize(1024);
  bool ret = g_mqtt_client.connect(
    g_persistent_data[PFIELD::my_hostname].c_str(),
    g_persistent_data[PFIELD::mqtt_username].c_str(),
    g_persistent_data[PFIELD::mqtt_password].c_str());
  if (ret)
    debug_println("MQTT client connected");
  else
    debug_println("MQTT client failed to connect, state: ", g_mqtt_client.state());

  // connect to the heatpump. Callbacks first so that the hpPacketDebug callback is available for connect()
  if (USE_HEATPUMP) {
    g_heat_pump.setSettingsChangedCallback([] () { send_hp_settings(g_heat_pump.getSettings()); });
    g_heat_pump.setStatusChangedCallback(send_hp_status);
    //g_heat_pump.setPacketCallback(hp_debug);
    g_heat_pump.enableExternalUpdate(); // IR remote settings will take effect
    g_heat_pump.enableAutoUpdate(); // calling sync() will propagate setSettings() call
    g_heat_pump.connect(&Serial);
  }
}

void loop(void) {
  g_server.handleClient();
  ArduinoOTA.handle();
  MDNS.update();
  if (g_mqtt_client.connected()) {
    g_mqtt_client.loop();
    if (USE_HEATPUMP) {
      // don't sync _too_ often, 100ms seems reasonable
      if (g_sync_timer.tick())      
        g_heat_pump.sync();
      // send status every so often, if sync didn't already do it for us due to a change
      if (g_send_timer.tick()) {
        send_hp_status(g_heat_pump.getStatus());
        send_hp_settings(g_heat_pump.getSettings());
      }
    }
    else {
      mock_heat_pump_sync();
    }
  }
}
