#pragma once

#include <Arduino.h>

namespace wifi_mgr {

void begin(const char* ssid, const char* pass);
bool connected();
String ip();

}  // namespace wifi_mgr
