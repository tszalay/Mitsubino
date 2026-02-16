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
//void handle_mqtt_message(char* topic, byte* payload, unsigned int length);

class MQTTStateMachine : public CRTPStateMachine<MQTTStateMachine, MQTTStates> {
  WiFiClient wifi_client_; // genuinely not sure what this does.
  String hostname_;
  Logger* logger_;

public:
  using CRTPStateMachine::state_t;

  PubSubClient client{wifi_client_};

  MQTTStateMachine(Logger* logger, String my_hostname, String server, String password, int port) : logger_(logger), hostname_(my_hostname) {
    client.setServer(server.c_str(), port);
    client.setBufferSize(1024);
  }

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
        if (client.connect(hostname_.c_str())) {
          transition(state_t::CONNECTED);
        }
        else {
          transition(state_t::DISCONNECTED);
          logger_->println("MQTT client failed to connect, state: ", client.state());
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
