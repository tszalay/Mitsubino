#include <WiFi.h>
#include <ESPmDNS.h>

#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <Adafruit_SHT4x.h>  // 1.0.4

#include "Logger.h"
#include "PersistentData.h"
#include "WifiStates.h"
#include "MQTTClient.h"
#include "HTTPConfigServer.h"
#include "ESPNOWMsg.h"

#ifdef ESP32

// Overall settings don't really matter, but to keep partition consistent:
// Use "default 4MB SPIFFS" partition if re-flashing, don't wipe
// Upload speed 921600, CPU 240MHz all seems to work

// Need to look up code for changing wifi channel properly and implement it in set next channel
// Order of deployment: (a) set up relay and test, (b) set up remote thermostat and test

// Notes on handling receiving messages: we have 3 cases.
// (a) response to recently sent message
// (b) message intended for us, as recipient
// (c) some sort of broadcast message (have flag to subscribe)
// so probably need a few receive buffers to make sure we catch them all
// temp sensors only need to handle (a) though, or generally anyone that is sleeping
// and as a rule we can say broadcast messages are push-only?
// how about sending messages to the relay? do we want to force everyone to remember
// the hostname of the relay or what? we can just hardcode it for now I guess, since
// it's only temporary until we flip the heatpump control over too.

// other misc TODO:
// really transition logger over to global Logger::get() and get rid of passing it around, that's silly
// (or just make a Logger::println that is similarly templated but only if we have a logger)

Logger g_logger{ 2048 };
PersistentData g_persistent_data(&g_logger);

WifiClientStateMachine* g_wifi = nullptr;
MQTTStateMachine* g_mqtt = nullptr;
HTTPConfigServer* g_server = nullptr;
ESPNOWStateMachine* g_espnow = nullptr;

Adafruit_SHT4x sht4{};
SimpleTimer g_temp_timer{ 15000 };

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

struct RTCData {
  // whether we are going back to sleep or not after waking.
  // defaults to connecting to wifi after a reset, but the response packet
  // will tell us what to do.
  bool sleepEnabled;
  // the most recently used wifi channel to avoid needing to do a scan
  int wifiChannel;
  // count the number of wakeups since last reset
  int numWakeups;

  // no constructor so it doesn't automatically run on wake
  void reset() {
    sleepEnabled = false;
    wifiChannel = 1;
    numWakeups = 0;
  }
};
RTC_NOINIT_ATTR RTCData g_rtcdata;

enum class MitsubinoRole : int {
  Heatpump,
  Relay,
  TemperatureSensor,
  RemoteControl,
  Unknown
};
MitsubinoRole g_role{ MitsubinoRole::Unknown };

void setup() {
  WiFi.mode(WIFI_OFF);
  Serial.begin(115200);

  g_logger.println("Starting setup");
  CRTPBase::logger_ = &g_logger;

  esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason != ESP_RST_DEEPSLEEP) {
    g_rtcdata.reset();
  }

  if (!g_persistent_data.load()) {
    g_logger.println("Failed to fully load persistent data:");
  } else {
    g_logger.println("Loaded persistent data:");
  }
  g_persistent_data.print();

  if (g_persistent_data.role == "heatpump") {
    g_role = MitsubinoRole::Heatpump;
  }
  if (g_persistent_data.role == "relay") {
    g_role = MitsubinoRole::Relay;
  }
  if (g_persistent_data.role == "temp_sensor") {
    g_role = MitsubinoRole::TemperatureSensor;
  }
  if (g_persistent_data.role == "remote") {
    g_role = MitsubinoRole::RemoteControl;
  }
  if (g_role == MitsubinoRole::Unknown) {
    g_logger.println("Unknown role: ", g_persistent_data.role);
  }

  // heatpumps talk over serial, not compatible with serial logging
  if (g_role != MitsubinoRole::Heatpump) {
    g_logger.set_serial(true);
    Serial.print(g_logger.get());
  }


  // only temperature sensors and remotes allowed to sleep
  if (g_role != MitsubinoRole::TemperatureSensor && g_role != MitsubinoRole::RemoteControl) {
    g_rtcdata.sleepEnabled = false;
  }
  if (g_rtcdata.numWakeups >= 5) {
    g_rtcdata.sleepEnabled = false;
  }

  if (!g_rtcdata.sleepEnabled) {
    g_wifi = new WifiClientStateMachine(&g_logger, g_persistent_data.my_hostname, g_persistent_data.ssid, g_persistent_data.password);

    ArduinoOTA.setPort(8266);
    ArduinoOTA.setHostname(g_persistent_data.my_hostname.c_str());

    MDNS.begin(g_persistent_data.my_hostname.c_str());
    g_server = new HTTPConfigServer(&g_logger, &g_persistent_data);
    ArduinoOTA.begin();

    g_mqtt = new MQTTStateMachine(&g_logger, g_persistent_data.my_hostname, g_persistent_data.mqtt_hostname, g_persistent_data.mqtt_username, g_persistent_data.mqtt_password, g_persistent_data.mqtt_port.toInt());
  }
  g_espnow = new ESPNOWStateMachine(&g_logger, g_persistent_data.my_hostname, !g_rtcdata.sleepEnabled, g_rtcdata.wifiChannel);

  WIRE_TO_USE.setPins(SDA_TO_USE, SCL_TO_USE);
  pinMode(BUTTON, INPUT);
}


void read_sensor() {
  g_logger.println("Free RAM: ", ESP.getFreeHeap());
  float uptime = millis();
  uptime /= (1000 * 60 * 60 * 24);
  g_logger.println("Uptime: ", uptime, " days");
  if (!sht4.begin(&WIRE_TO_USE)) {
    g_logger.println("Couldn't find SHT4x");
  } else {
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
  if (g_persistent_data.my_hostname == "remote_temp_1") {
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

void disableInternalPower() {
#if defined(NEOPIXEL_POWER)
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, LOW);
#endif

#if defined(NEOPIXEL_I2C_POWER)
  pinMode(NEOPIXEL_I2C_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_I2C_POWER, LOW);
#endif
}

SimpleTimer g_espnow_timer{ 5000 };

void loop() {
  /*if (g_espnow_timer.tick()) {
    g_logger.println("Sending ESPNOW message");
    String msg = "Hello from ";
    msg += g_persistent_data.my_hostname;
    msg += " at time " + String(millis());
    esp_now_manual_xor(msg);
    if (esp_now_send(ESP_NOW_BROADCAST_MAC, (const uint8_t*)msg.c_str(), msg.length()+1) != ESP_OK) {
      g_logger.println("esp_now_send failed!");
    }
  }*/
  if (g_wifi) {
    g_wifi->loop();
  }
  if (g_server) {
    g_server->loop();
    ArduinoOTA.handle();
  }
  if (g_mqtt) {
    g_mqtt->loop();
  }
  g_espnow->loop();
  if (g_espnow->hasReceived()) {
    auto msg = g_espnow->getReceived();
    switch (g_role) {
      case MitsubinoRole::Relay:
        if (msg.type == ReceivedMessage::Type::Unicast) {
          g_logger.println("Got unicast message from ", msg->sender, ": ", msg.body().c_str(), ", sending response");
          MsgMQTTRelay response{};
          response.sender = g_persistent_data.my_hostname;
          response.recipient = msg->sender;
          response.seqnum = msg->seqnum;
          response.body = String("responding to hellow world");
          const char* mstart = (const char*)&response;
          g_espnow->sendResponse(String(mstart, sizeof(response)));
        }
        break;
      case MitsubinoRole::TemperatureSensor:
        if (msg.type == ReceivedMessage::Type::Response) {
          g_logger.println("Got response in ", g_espnow_timer.value(), "ms from ", msg->sender, ": ", msg.body().c_str());
        }
        if (g_rtcdata.sleepEnabled) {
          g_logger.println("Going to sleep");
          g_rtcdata.numWakeups++;
          g_rtcdata.wifiChannel = g_espnow->getChannel();
          esp_sleep_enable_timer_wakeup(5000000);
          disableInternalPower();
          esp_deep_sleep_start();
        }
        break;
    }
  }
  if (g_role == MitsubinoRole::TemperatureSensor && g_espnow->canSend()) {
    if (!digitalRead(BUTTON)) {
      g_rtcdata.sleepEnabled = true;
      g_rtcdata.numWakeups = 0;
      g_logger.println("Setting sleep mode enabled");
    }
    if (g_rtcdata.sleepEnabled || g_espnow_timer.tick()) {
      MsgMQTTRelay msg{};
      msg.sender = g_persistent_data.my_hostname;
      msg.recipient = "hp_relay";
      msg.seqnum = 11;
      msg.body = String("greetings from temp sensor at ") + String(millis()) + String(" after attempts: ") + String(g_espnow->numAttempts());
      const char* mstart = (const char*)&msg;
      g_espnow->sendMessage(String(mstart, sizeof(msg)));
      g_logger.println("Sent to relay");
    }
  }
}

#endif