#pragma once

#include <WiFi.h>
#include <ArduinoJson.h>   // ArduinoJson library v6.21.4
#include <PubSubClient.h>  // PubSubClient library v2.8.0

#include "States.h"

enum class MQTTStates : int {
  DISCONNECTED,
  CONNECTING,
  CONNECTED,
};

//  bool connect() {
//    g_mqtt_client.setCallback(handle_mqtt_message);
//    g_mqtt_client.subscribe(get_topic_name("control").c_str());
//  }
void handle_mqtt_message(char* topic, byte* payload, unsigned int length);

class MQTTStateMachine : public CRTPStateMachine<MQTTStateMachine, MQTTStates> {
  WiFiClient wifi_client_; // genuinely not sure what this does.
  const String hostname_;
  const String server_;
  const String username_;
  const String password_;
  Logger* logger_;

public:
  using CRTPStateMachine::state_t;

  PubSubClient client{wifi_client_};

  MQTTStateMachine(Logger* logger, String my_hostname, String server, String username, String password, int port) : logger_(logger), hostname_(my_hostname), server_(server), username_(username), password_(password) {
    client.setServer(server_.c_str(), port);
    client.setBufferSize(1024);
    client.setCallback(handle_mqtt_message);
  }

  static constexpr const char* name = "MQTT";
  static constexpr state_t initial_state = state_t::DISCONNECTED;

private:
  void disconnect() {
    client.disconnect();
    transition(state_t::DISCONNECTED);
  }

public:
  void loopImpl() {
    switch (state()) {
      case state_t::CONNECTED:
        if (WiFi.status() != WL_CONNECTED || !client.connected())
          disconnect();
        else
          client.loop();
        return;
      case state_t::CONNECTING:
        if (time_in_state() < 200)
          return;
        if (WiFi.status() != WL_CONNECTED) {
          disconnect();
          return;
        }
        if (client.connect(hostname_.c_str(), username_.c_str(), password_.c_str())) {
          logger_->println("MQTT connected");
          transition(state_t::CONNECTED);
        }
        else {
          logger_->println("MQTT client failed to connect, state: ", client.state());
          disconnect();
        }
        return;
      case state_t::DISCONNECTED:
        if (time_in_state() < 100)
          return;
        if (WiFi.status() == WL_CONNECTED)
          transition(state_t::CONNECTING);
        return;
    }
  }
};
