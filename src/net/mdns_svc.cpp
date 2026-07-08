#include "mdns_svc.h"

#include <Arduino.h>
#include <ESPmDNS.h>

#include "../config.h"
#include "device_identity.h"

namespace mdns_svc {

namespace {
bool g_started = false;
}

void begin() {
  if (g_started) return;

  String host = device_identity::deviceId();
  if (!MDNS.begin(host.c_str())) {
    log_w("[mdns] responder failed to start");
    return;
  }

  MDNS.addService("http", "tcp", 80);

  // The Rozakos Home app browses _rozakos._tcp and reads these TXT records to
  // list the device without probing; keep keys in sync with the app's
  // NsdDeviceDiscoveryService and docs/device-discovery-protocol.md.
  MDNS.addService("rozakos", "tcp", 80);
  MDNS.addServiceTxt("rozakos", "tcp", "id",   host.c_str());
  MDNS.addServiceTxt("rozakos", "tcp", "type", "cyd_stock_ticker");
  MDNS.addServiceTxt("rozakos", "tcp", "name", "CYD Stock Ticker");
  MDNS.addServiceTxt("rozakos", "tcp", "fw",   cfg::FW_VERSION);
  MDNS.addServiceTxt("rozakos", "tcp", "mac",  device_identity::macString().c_str());
  MDNS.addServiceTxt("rozakos", "tcp", "path", "/settings");

  g_started = true;
  log_i("[mdns] advertising %s.local (_rozakos._tcp on 80)", host.c_str());
}

}  // namespace mdns_svc
