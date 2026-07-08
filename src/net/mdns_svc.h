#pragma once

namespace mdns_svc {

// Starts the mDNS responder and advertises the device on the LAN. Call once
// the STA link is up (alongside web_admin::begin). Idempotent; safe to call
// again after a WiFi drop/reconnect. Advertises:
//   - hostname  <deviceId>.local (e.g. cyd-ticker-ab12.local)
//   - _http._tcp    port 80 (generic browsers)
//   - _rozakos._tcp port 80 with TXT id/type/fw/mac/path — the service the
//     Rozakos Home app discovers. See docs/device-discovery-protocol.md.
void begin();

}  // namespace mdns_svc
