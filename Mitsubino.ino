#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <DNSServer.h>

#define CONFIG_AP_NAME "Mitsubino-Config"

ESP8266WebServer server(80);

// Helpers to load and save relevant data, sorry about all the macros
// Change this to add fields
#define FOR_ALL_FIELDS(FUN) \
  FUN(ssid)                 \
  FUN(password)             \
  FUN(my_hostname)          \
  FUN(mqtt_hostname)

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
  for (int i=0; i<data.size(); i++) {
    String name = String(PERSISTENT_FIELD_NAMES[i]);
    File f = LittleFS.open("/" + name, "r");
    if (!f) {
      Serial.println("File " + name + " could not be read");
      return false;
    }
    data[i] = f.readString();
  }
  LittleFS.end();
  return true;
}

bool save_persistent_data(PersistentData& data) {
  LittleFS.begin();
  for (int i=0; i<data.size(); i++) {
    String name = String(PERSISTENT_FIELD_NAMES[i]);
    File f = LittleFS.open("/" + name, "w");
    if (!f) {
      Serial.println("File " + name + " could not be opened for write");
      return false;
    }
    f.print(data[i]);
  }
  LittleFS.end();
  return true;
}

void print_persistent_data(PersistentData& data) {
  for (int i=0; i<data.size(); i++)
    Serial.println(String(PERSISTENT_FIELD_NAMES[i]) + " = " + data[i]);
}

// Shows webpage that displays forms to submit
void handle_persistent_forms() {
  PersistentData data;
  // use whatever we have saved to try and prepopulate the fields
  load_persistent_data(data);
  String content = F("<!DOCTYPE HTML>\r\n<html>Mitsubino Connectivity Setup <form method='get' action='save'>");
  for (int i=0; i<data.size(); i++) {
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
  for (int i=0; i<data.size(); i++)
    data[i] = server.arg(PERSISTENT_FIELD_NAMES[i]);
  Serial.println("Received data from POST and saving to Flash:");
  print_persistent_data(data);
  save_persistent_data(data);
  server.send(200, "text/html", F("Data saved, rebooting. You may need to change networks or addresses to reconnect."));
  Serial.println("Rebooting...");
  delay(1000);
  ESP.restart();
}

void blink_once() {
    digitalWrite(LED_BUILTIN, LOW);  // turn the LED on (HIGH is the voltage level)
    delay(1000);                      // wait for a second
    digitalWrite(LED_BUILTIN, HIGH);   // turn the LED off by making the voltage LOW
}

void start_server() {
  server.on("/", handle_persistent_forms);
  server.on("/save", handle_persistent_save);
  server.on("/blink", []() {
    blink_once();
    server.send(200, "text/plain", "blinking");
  });
  server.onNotFound([] () {
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

// Set up an access point with SSID Mitsubino and PW Mitsubino
void start_ap_and_server() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.softAP(CONFIG_AP_NAME, "");
  start_server();
  Serial.println(CONFIG_AP_NAME " wifi network started");
  DNSServer dnsServer;
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());
  // just in case I need to reflash while in disconnected mode
  ArduinoOTA.begin();
  auto start_millis = millis();
  // only serve the access point for 10 minutes, then go into suspend
  auto end_millis = start_millis + 10*60*1000;
  while (millis() < end_millis) {
    server.handleClient();
    ArduinoOTA.handle();
    dnsServer.processNextRequest();
  }
  Serial.println("No client or reconfiguration received, suspending...");
  WiFi.disconnect();
  while (true)
    blink_once();
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  Serial.println("");
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  
  PersistentData data;
  if (!load_persistent_data(data)) {
    Serial.println("Failed to load persistent data, serving access point with config page");
    start_ap_and_server();
  }

  Serial.println("Loaded persistent data:");
  print_persistent_data(data);

  // WiFi settings
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setPhyMode(WIFI_PHY_MODE_11G);
  WiFi.begin(data[PFIELD::ssid], data[PFIELD::password]);

  // OTA update settings
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(data[PFIELD::my_hostname].c_str());

  int seconds_waiting = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    seconds_waiting++;
    if (seconds_waiting > 60) {
      Serial.println("Not connected in 60s, serving access point with config page");
      // this will loop forever and restart
      start_ap_and_server();
    }
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(WiFi.SSID());
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  MDNS.begin(data[PFIELD::my_hostname].c_str());
  Serial.println("MDNS started");
  start_server();
  Serial.println("HTTP server started");
  ArduinoOTA.begin();
  Serial.println("OTA server started");
}

void loop(void) {
  server.handleClient();
  ArduinoOTA.handle();
  MDNS.update();
}
