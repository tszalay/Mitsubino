#pragma once

#ifdef ESP8266
#include <ESP8266WebServer.h>
#else
#include <WebServer.h>
#endif

#include "Logger.h"
#include "PersistentData.h"

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

class HTTPConfigServer {
  #ifdef ESP8266
  ESP8266WebServer server_{80};
  #else
  WebServer server_{80};
  #endif

  Logger* logger_;
  PersistentData* persistent_data_;

public:
  HTTPConfigServer(Logger* logger, PersistentData* persistent_data) : logger_{logger}, persistent_data_{persistent_data} {
    server_.on("/", [this] () { server_.send(200, "text/html", String(ROOT_PAGE_BODY)); });
    server_.on("/config", [this] () { handle_persistent_forms(); });
    server_.on("/save", [this] () { handle_persistent_save(); });
    server_.on("/log", [this] () { server_.send(200, "text/html", String(LOG_PAGE_BODY)); });
    server_.on("/get_log", [this] () {
      server_.send(200, "text/plain", logger_->get());
      logger_->clear();
    });
    server_.on("/restart", [this] () { server_.send(200, "text/plain", "Restarting..."); ESP.restart(); });
    server_.onNotFound([this]() {
      String message = "File Not Found\n\n";
      message += "URI: ";
      message += server_.uri();
      message += "\nMethod: ";
      message += (server_.method() == HTTP_GET) ? "GET" : "POST";
      message += "\nArguments: ";
      message += server_.args();
      message += "\n";
      for (uint8_t i = 0; i < server_.args(); i++) { message += " " + server_.argName(i) + ": " + server_.arg(i) + "\n"; }
      server_.send(404, "text/plain", message);
    });
    server_.begin();
  }

  void loop() {
    server_.handleClient();
  }

private:

  // Shows webpage that displays forms to submit
  void handle_persistent_forms() {
    // use whatever we have saved to try and prepopulate the fields
    String content = F("<!DOCTYPE HTML>\r\n<html>Mitsubino Connectivity Setup <form method='get' action='save'>");
    for (size_t i = 0; i < PersistentData::NumFields; i++) {
      content += "<label>" + String(PersistentData::FieldNames[i]) + ": </label>";
      content += "<input name = '" + String(PersistentData::FieldNames[i]) + "' ";
      content += " value = '" + persistent_data_->fields()[i] + "' length=64><br>";
    }
    content += "<input type='submit'></form></html>";
    server_.send(200, "text/html", content);
  }

  // Saves the resulting POST from form submission
  void handle_persistent_save() {
    PersistentData data(logger_);
    for (int i = 0; i < PersistentData::NumFields; i++)
      data.fields()[i] = server_.arg(PersistentData::FieldNames[i]);
    logger_->println("Received data from POST and saving to Flash:");
    data.print();
    data.save();
    server_.send(200, "text/html", F("Data saved, rebooting. You may need to change networks or addresses to reconnect."));
    logger_->println("Rebooting...");
    delay(1000);
    ESP.restart();
  }
};