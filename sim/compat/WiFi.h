#pragma once
// Pretend we're connected to a known network. Used by the status bar / settings
// screen for display purposes only.

#include "Arduino.h"

#define WL_CONNECTED 3

struct IPAddressStub {
  String toString() const { return String("192.168.1.50"); }
};

class WiFiStub {
 public:
  int           status()   const { return WL_CONNECTED; }
  long          RSSI()     const { return -55; }
  String        SSID()     const { return String("SimNet"); }
  IPAddressStub localIP()  const { return {}; }
};

extern WiFiStub WiFi;
