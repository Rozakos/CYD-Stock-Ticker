// Sim stub for wifi_mgr — the device build uses the real one in
// src/net/wifi_mgr.cpp (depends on Arduino-ESP32 WiFi.h, which we don't
// have on host). The wifi setup screen only reads connection state, so a
// fixed "AP active, IP 192.168.4.1" is enough to render the screen.

#include "Arduino.h"
#include "../src/net/wifi_mgr.h"

namespace wifi_mgr {

void begin(SettingsStore&) {}
void retrySta(SettingsStore&) {}

bool   connected() { return false; }
bool   apActive()  { return true; }
StaStatus   staStatus()      { return StaStatus::Idle; }
const char* lastFailMessage() { return ""; }
String ip()        { return String("192.168.4.1"); }
String apSsid()    { return String("CYD-Setup-AB12"); }
String apPass()    { return String(); }  // open AP

}  // namespace wifi_mgr
