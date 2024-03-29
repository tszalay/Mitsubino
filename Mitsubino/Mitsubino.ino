
// only use this code if we are compiling for the heat pumps
#ifdef ESP8266

// Partition config:
// Flash Size 4MB, FS: 2MB, OTA ~1MB

#include "MitsubinoShared.h"

#include "src/HeatPump/src/HeatPump.h"      // SwiCago, submodule installed in Arduino library dir

// whether we are plugged into USB or the heat pump.
// this is compile-time const because if we reconnect to the USB serial
// we may as well re-flash, ESP8266 only has one real serial port.
static const bool USE_HEATPUMP = true;

HeatPump g_heat_pump;
float g_fudge = 0.01; // fudge temp by a tiny bit so that HA shows distinct values

SimpleTimer g_send_timer{15000}; // updates
SimpleTimer g_log_raw_timer{60000}; // logging of raw packets, when enabled
SimpleTimer g_last_settings_timer{15000}; // last time we wrote settings, for disabling callback
unsigned long g_last_remote_temp_write = 0; // less simple than SimpleTimer
const unsigned long REMOTE_TEMP_TIMEOUT = 300*1000;
bool g_message_received_this_round = false;

void send_hp_data(const heatpumpSettings& settings, const heatpumpStatus& status) {
  if (!settings.connected || status.roomTemperature == 0) {
    debug_println("Skipping send due to invalid data");
    return;
  }
  bool sent = false;
  {
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

    if (!g_mqtt_client.publish(get_topic_name("settings").c_str(), s.c_str(), true)) {
      debug_print("Failed to publish settings change to settings topic");
    }
    else {
      sent = true;
      debug_println("sent settings: ", s);
    }
  }

  {
    // send room temp and operating info
    DynamicJsonDocument msg(JSON_OBJECT_SIZE(3));
    msg["roomTemperature"] = status.roomTemperature;
    msg["operating"] = status.operating;
    msg["wiggle"] = g_fudge;

    // invert fudge for next time
    g_fudge = -g_fudge;

    String s;
    serializeJson(msg, s);
    if (!g_mqtt_client.publish(get_topic_name("status").c_str(), s.c_str(), true)) {
      debug_println("failed to publish to room temp and operation status to status topic");
    }
    else {
      sent = true;
      debug_println("sent status: ", s);
    }
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
  if (sent) {
    g_send_timer.reset();
    g_reset_timer.reset();
  }
}

void send_hp_data() {
  send_hp_data(g_heat_pump.getSettings(), g_heat_pump.getStatus());
}

void hp_debug(byte* packet, unsigned int length, char* packetDirection) {
  if (String(packetDirection) == String("packetSent") || !g_log_raw_timer.tick())
    return;
  debug_println("Heatpump packet of length ", length, " with direction ", packetDirection);
  for (int idx = 0; idx < length; idx++) {
    if (packet[idx] < 16)
      debug_print("0"); // pad single hex digits with a 0
    debug_print(String(packet[idx], HEX), " ");
  }
  debug_print("\n");
}

void handle_mqtt_message(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != get_topic_name("control")) {
    debug_println("Received message at unrecognized topic: ", topic);
    return;
  }

  // this is probably fine
  String message;
  message.concat((const char*)payload, length);
  debug_println("Got MQTT message on topic ", topic, ": ", message);
  DynamicJsonDocument root(JSON_OBJECT_SIZE(6));
  DeserializationError error = deserializeJson(root, message.c_str());

  if (error) {
    debug_println("Error parsing received mqtt message: ", error.c_str());
    return;
  }

  if (root.containsKey("power")) {
    g_heat_pump.setPowerSetting((const char*)root["power"]);
    // reflect this back to HA right away, updating only the power field
    // stupid requirement of HA MQTT switches but not the other entities
    g_mqtt_client.publish(get_topic_name("settings").c_str(), message.c_str(), true);
  }
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
  if (root.containsKey("remoteTemp")) {
    float remote_temp = root["remoteTemp"];
    g_heat_pump.setRemoteTemperature(remote_temp);
    debug_println("remote temp set to ", remote_temp);
    g_last_remote_temp_write = millis();
    return; // packet write was immediate, no need to do anything else
  }

  // update time that disables callbacks
  // reset send timer to give everything 15s to settle before we send back updates
  g_last_settings_timer.reset();
  g_send_timer.reset();
  g_reset_timer.reset();
  g_message_received_this_round = true;
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
  send_hp_data(settings, status);
}

void setup() {
  // Serial is either hooked up to heatpump or USB
  if (!USE_HEATPUMP) {
    Serial.begin(115200);
    Serial.println("");
  }
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  // connect to wifi, mDNS, OTA, MQTT, start server, etc
  configure_shared();

  // connect to the heatpump. Callbacks first so that the hpPacketDebug callback is available for connect()
  if (USE_HEATPUMP) {
    g_heat_pump.setSettingsChangedCallback([] () {
      debug_println("Got external update");
      if (g_last_settings_timer.peek())
        send_hp_data();
      else
        debug_println("Skipping update because we triggered it");
    });
    g_heat_pump.setStatusChangedCallback([] (heatpumpStatus) { send_hp_data(); });
    g_heat_pump.setPacketCallback(hp_debug);
    g_heat_pump.setOnConnectCallback([] () { debug_println("Connected to heatpump"); });
    g_heat_pump.enableExternalUpdate(); // IR remote settings will take effect
    g_heat_pump.enableAutoUpdate(); // calling sync() will propagate setSettings() call
    g_heat_pump.connect(&Serial);
  }
}

void loop() {
  loop_shared();
  // doesn't matter if MQTT is down, if remote temp is lost, we cancel that.
  // looks like this is an immediate write, no waiting for sync.
  if (g_last_remote_temp_write != 0 && millis() - g_last_remote_temp_write > REMOTE_TEMP_TIMEOUT) {
    g_heat_pump.setRemoteTemperature(0);
    debug_println("Reverting to internal thermostat due to remote temp timeout");
    g_last_remote_temp_write = 0;
  }
  if (g_mqtt_client.connected()) {
    g_message_received_this_round = false;
    g_mqtt_client.loop();
    // only call sync() if we didn't get an MQTT message this round, or else we update once
    // per MQTT message which really slows things down for multiple button presses
    if (USE_HEATPUMP && !g_message_received_this_round) {
      auto start = millis();
      g_heat_pump.sync();
      auto dt = millis() - start;
      if (dt > 2000)
        debug_println("Sync took ", dt, " ms");
      // send status every so often, if sync didn't already do it for us due to a change
      if (g_send_timer.tick())
        send_hp_data();
    }
    else {
      mock_heat_pump_sync();
    }
  }
  else {
    debug_println("MQTT disconnected, attempting reconnect...");
    mqtt_connect();
  }
}

#endif