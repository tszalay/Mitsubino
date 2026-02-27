#pragma once

#include <algorithm>
#include <string_view>

#include <esp_wifi.h>
#include <ESP32_NOW.h>
#include <esp_now.h>
#include <esp_mac.h>  // For the MAC2STR and MACSTR macros

#include "States.h"

const uint8_t ESP_NOW_BROADCAST_MAC[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// we are using manual encryption because broadcast mode doesn't support encryption
static const char* ESP_NOW_MANUAL_KEY = "Bite my shiny metal ass";

constexpr FixedString<16> BROADCAST_HOSTNAME(std::string_view("broadcast\0\0\0\0\0\0"));

enum class MsgID : uint16_t {
  NONE,
  MQTT,
  MQTT_RESP,
  ACK,
};

struct MsgHeader {
  // must match or message is discarded
  static constexpr uint8_t VERSION = 1;

  MsgID msgid{MsgID::NONE};
  const uint8_t version{VERSION};
  const uint8_t checksum;
  uint32_t seqnum;
  FixedString<16> sender;
  FixedString<16> recipient;
};

struct MsgMQTTRelay : public MsgHeader {
  // we have plenty of space. encoded JSON to pass to MQTT
  // note this should be a nested JSON object of the form
  // { "topic":topic, "message":message }
  FixedString<1024> body;
};

struct ReceivedMessage {
  enum class Type : int {
    Unset,
    Response,
    Broadcast,
    Unicast
  };

  Type type;
  String data;

  MsgHeader* operator->() const {
    return (MsgHeader*)data.begin();
  }

  operator bool() const {
    return data.length();
  }

  String body() const {
    return data.substring(sizeof(MsgHeader));
  }
};

/*inline void esp_now_manual_xor(String& msg) {
  const char* k = ESP_NOW_MANUAL_KEY;
  for (char& c : msg) {
    c ^= *k;
    if (*(++k) == 0)
      k = ESP_NOW_MANUAL_KEY;
  }
}*/
inline void esp_now_manual_xor(String& msg) {}

enum class ESPNOWStates : int {
  CONNECTING,
  READY_NO_ACK, // can send messages but have not gotten response
  CONNECTED,    // can send messages, have gotten response, on the right channel
  TRANSMIT,     // transmit and go to awaiting
  WAIT_ACK,     // awaiting ACK
  NEXT_CHANNEL, // short pause after switching channels
  FAILED,       // too many attempts
};

static constexpr std::array<int, 11> WIFI_CHANNELS{1, 6, 11, 5, 4, 3, 2, 7, 8, 9, 10};

/// Notes on the ESPNOW states:
/// - We are assuming that anything sending via ESPNOW is doing so transiently,
///   ie waking up from sleep, sending some messages, waiting for ACKs/responses,
///   and then going back to sleep
/// - This means we do things like search for the right channel on wake and then
///   can assume it persists for the lifetime of this class

class ESPNOWStateMachine : public CRTPStateMachine<ESPNOWStateMachine, ESPNOWStates> {
  Logger* logger_;
  FixedString<16> my_hostname_{};
  const bool wifiConnection_;
  String sendBuffer_;
  size_t numAttempts_{};

  std::vector<ReceivedMessage> receivedMessages_;

  static ESPNOWStateMachine* singleton_;
  using CRTPStateMachine::state_t;


public:
  static constexpr const char* name = "ESPNOW";
  static constexpr state_t initial_state = state_t::CONNECTING;

  int numAttempts() {
    return numAttempts_;
  }

  ESPNOWStateMachine(Logger* logger, String my_hostname, bool wifiConnection, int initialChannel) 
    : logger_{logger}, my_hostname_(my_hostname), wifiConnection_(wifiConnection) {
    // save for comms protocol
    // save for callbacks
    ESPNOWStateMachine::singleton_ = this;
    // Initialize wifi if we are in charge of it
    if (!wifiConnection) {
      WiFi.mode(WIFI_STA);
      setChannel(initialChannel);
    }
    // Initialize the ESP-NOW protocol
    if (esp_now_init() != ESP_OK) {
      logger_->println("ESP-NOW failed to init");
      //ESP.restart();
    }
    else {
      logger_->println("Initialized ESP-NOW");
      uint8_t baseMac[6];
      if (esp_wifi_get_mac(WIFI_IF_STA, baseMac) == ESP_OK) {
        logger_->println("WiFi MAC address: ", mac2str(baseMac));
      }
      esp_now_register_send_cb(onDataSent);
      esp_now_register_recv_cb(onDataReceived);
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, ESP_NOW_BROADCAST_MAC, 6);
      peerInfo.channel = 0; // Use channel 0 for default
      peerInfo.encrypt = false;
      auto ret = esp_now_add_peer(&peerInfo);
      if (ret == ESP_OK)
        logger_->println("Successfully added peer");
      else
        logger_->println("Failed to add peer: ", ret);
    }


    uint32_t esp_now_version = 1;
    auto err = esp_now_get_version(&esp_now_version);
    if (err != ESP_OK) {
      esp_now_version = 1;
    }
    const uint32_t max_data_len = (esp_now_version == 1) ? ESP_NOW_MAX_DATA_LEN : ESP_NOW_MAX_DATA_LEN_V2;
    logger_->println("ESP-NOW version: ", esp_now_version, ", max data length: ", max_data_len);
  }

  void loopImpl() {
    switch (state()) {
      case state_t::CONNECTING:
        if (wifiConnection_ && WiFi.status() == WL_CONNECTED) {
          // if we are running with stable wifi connection, we don't need to
          // seek channels, this will indicate that we are locked. but we won't
          // really be initiating any sends.
          transition(state_t::CONNECTED);
        }
        else if (!wifiConnection_ && WiFi.STA.started()) {
          // if we are running with boot-and-send, make sure we have a channel locked
          transition(state_t::READY_NO_ACK);
        }
        break;
      case state_t::TRANSMIT:
        esp_now_send(ESP_NOW_BROADCAST_MAC, (const uint8_t*)sendBuffer_.begin(), sendBuffer_.length());
        numAttempts_++;
        transition(state_t::WAIT_ACK);
        break;
      case state_t::WAIT_ACK:
        // timeout waiting for an ack. successful ack handled in on_data_received.
        if (time_in_state() > 200) {
          if (!wifiConnection_) {
            setNextChannel();
            transition(state_t::NEXT_CHANNEL);
          }
          else {
            transition(numAttempts_ < 10 ? state_t::TRANSMIT : state_t::FAILED);
          }
        }
        break;
      case state_t::NEXT_CHANNEL:
        if (time_in_state() > 4) {
          transition(state_t::TRANSMIT);
        }
        break;
      case state_t::CONNECTED:
      case state_t::READY_NO_ACK:
      case state_t::FAILED:
        // waiting for a sendMessage call
        break;
    }
  }

  bool canSend() const {
    return sendBuffer_.isEmpty() && state() != state_t::CONNECTING;
  }

  void sendMessage(String msg) {
    if (!canSend() || state() == state_t::CONNECTING) {
      return;
    }
    sendBuffer_ = std::move(msg);
    esp_now_manual_xor(sendBuffer_);
    numAttempts_ = 0;
    transition(state_t::TRANSMIT);
  }

  void sendResponse(String msg) {
    // fire-and-forget, we're not waiting for a response here
    esp_now_manual_xor(msg);
    esp_now_send(ESP_NOW_BROADCAST_MAC, (const uint8_t*)msg.begin(), msg.length());
  }

  bool hasReceived() const {
    return receivedMessages_.size();
  }

  ReceivedMessage getReceived() {
    ReceivedMessage recv = std::move(receivedMessages_[0]);
    receivedMessages_.erase(receivedMessages_.begin());
    return recv;
  }

  int getChannel() {
    uint8_t chan;
    wifi_second_chan_t chan2;
    esp_wifi_get_channel(&chan, &chan2);
    return chan;
  }

  void setChannel(int channel) {
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
    logger_->println("Setting wifi channel to ", channel);
  }

  void setNextChannel() {
    int channel = getChannel();
    auto it = std::find(WIFI_CHANNELS.begin(), WIFI_CHANNELS.end(), channel);
    int nextIndex = (it - WIFI_CHANNELS.begin()) + 1;
    setChannel(WIFI_CHANNELS[(nextIndex+1) % WIFI_CHANNELS.size()]);
  }

private:
  void onReceive(String message) {
    ReceivedMessage msg{ReceivedMessage::Type::Unset, std::move(message)};
    if (msg->version != MsgHeader::VERSION) {
      logger_->println("Discarding packet due to version mismatch, got ", msg->version, " but expected ", MsgHeader::VERSION);
      return;
    }
    bool isForMe = msg->recipient == my_hostname_;
    bool isBroadcast = msg->recipient == BROADCAST_HOSTNAME;
    if (!(isForMe || isBroadcast)) {
      logger_->println("Discarding packet because recipient is ", msg->recipient, " but expected ", my_hostname_);
      return;
    }
    MsgHeader* sentMsg = sendBuffer_.length() ? (MsgHeader*)sendBuffer_.begin() : nullptr;
    bool isResponse = sentMsg && (msg->seqnum == sentMsg->seqnum) && (msg->sender == sentMsg->recipient);
    if (isForMe) {
      if (isResponse) {
        msg.type = ReceivedMessage::Type::Response;
        bool correctState = state() == state_t::WAIT_ACK;
        bool correctSend = sendBuffer_.length() > 0;
        if (!(correctState && correctSend)) {
          logger_->println("Error: received unexpected message, code: ", correctState, correctSend);
        }
        sendBuffer_.clear();
        transition(state_t::CONNECTED);
      }
      else {
        msg.type = ReceivedMessage::Type::Unicast;
      }
    }
    else {
      // must be broadcast
      msg.type = ReceivedMessage::Type::Broadcast;
    }
    receivedMessages_.push_back(std::move(msg));
  }

  static void onDataSent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    // it's assumed that this will succeed
    ESPNOWStateMachine::singleton_->logger_->println("Packet send status: ", (status == ESP_NOW_SEND_SUCCESS) ? "success" : "failure");
  }

  static void onDataReceived(const esp_now_recv_info_t *rx_info, const uint8_t *incomingData, int len) {
    String msg((const char*)incomingData, len);
    esp_now_manual_xor(msg);
    ESPNOWStateMachine::singleton_->onReceive(std::move(msg));
  }
};

ESPNOWStateMachine* ESPNOWStateMachine::singleton_ = nullptr;
