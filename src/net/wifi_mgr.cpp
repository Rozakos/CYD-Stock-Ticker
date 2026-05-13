#include "wifi_mgr.h"

#include <WiFi.h>

namespace wifi_mgr {

void begin(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid, pass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

bool connected() {
  return WiFi.status() == WL_CONNECTED;
}

String ip() {
  return WiFi.localIP().toString();
}

}  // namespace wifi_mgr
