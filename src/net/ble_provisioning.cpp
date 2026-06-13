#include "ble_provisioning.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <esp_mac.h>

#include "../config.h"

namespace ble_prov {

namespace {

// 128-bit protocol UUIDs — must match the Android app byte-for-byte. See
// docs/ble-provisioning-protocol.md. Do not change these.
constexpr char SVC_UUID[]    = "5f1d0001-7b3a-4c2e-9d8f-1a2b3c4d5e6f";
constexpr char CRED_UUID[]   = "5f1d0002-7b3a-4c2e-9d8f-1a2b3c4d5e6f";
constexpr char STATUS_UUID[] = "5f1d0003-7b3a-4c2e-9d8f-1a2b3c4d5e6f";
constexpr char INFO_UUID[]   = "5f1d0004-7b3a-4c2e-9d8f-1a2b3c4d5e6f";

NimBLEServer*         g_server     = nullptr;
NimBLECharacteristic* g_credChar   = nullptr;
NimBLECharacteristic* g_statusChar = nullptr;
NimBLECharacteristic* g_infoChar   = nullptr;

// Guards the cross-task shared state below: g_pending* (written on the BLE
// host task, read on the net task) and g_statusJson (written on the net task,
// read on the BLE host task in onSubscribe).
SemaphoreHandle_t g_mu = nullptr;

bool   g_pending      = false;
String g_pendingSsid;
String g_pendingPass;
String g_statusJson;     // last published status JSON (cached for Read/subscribe)

volatile bool g_active        = false;
volatile bool g_advertising   = false;
volatile bool g_clientConn    = false;

String buildStatus(const char* state, const String& ip, const char* reason) {
  JsonDocument d;
  d["state"]  = state;
  d["ip"]     = ip;
  d["reason"] = reason ? reason : "";
  String out;
  serializeJson(d, out);
  return out;
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    g_clientConn  = true;
    g_advertising = false;  // NimBLE halts advertising on connect
    log_i("[ble] central connected");
  }
  void onDisconnect(NimBLEServer*) override {
    g_clientConn  = false;
    g_advertising = false;  // the net task re-arms advertising if still needed
    log_i("[ble] central disconnected");
  }
};

class CredCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string raw = c->getValue();
    log_i("[ble] credentials write (%u bytes)", (unsigned)raw.size());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, raw);
    if (err) {
      log_w("[ble] credentials JSON parse failed: %s", err.c_str());
      return;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["pass"] | "";
    if (!ssid[0]) {
      log_w("[ble] credentials missing ssid");
      return;
    }
    xSemaphoreTake(g_mu, portMAX_DELAY);
    g_pendingSsid = ssid;
    g_pendingPass = pass;
    g_pending     = true;
    xSemaphoreGive(g_mu);
  }
};

class StatusCallbacks : public NimBLECharacteristicCallbacks {
  // When a central subscribes to notifications, push the current status right
  // away. This is what lets an app that connects to an already-provisioned
  // device immediately learn it's "connected" with its IP.
  void onSubscribe(NimBLECharacteristic* c, ble_gap_conn_desc* /*desc*/,
                   uint16_t subValue) override {
    if (!(subValue & 0x0001)) return;  // notifications not enabled
    String snapshot;
    xSemaphoreTake(g_mu, portMAX_DELAY);
    snapshot = g_statusJson;
    xSemaphoreGive(g_mu);
    if (snapshot.length()) {
      c->setValue((uint8_t*)snapshot.c_str(), snapshot.length());
      c->notify();
      log_i("[ble] pushed status on subscribe: %s", snapshot.c_str());
    }
  }
};

String makeLocalName(const uint8_t mac[6]) {
  char buf[20];
  snprintf(buf, sizeof(buf), "CYD-Ticker-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

String makeMacString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String buildDeviceInfo(const String& macStr) {
  JsonDocument d;
  d["name"] = "CYD Stock Ticker";
  d["type"] = "cyd_stock_ticker";
  d["fw"]   = cfg::FW_VERSION;
  d["mac"]  = macStr;
  String out;
  serializeJson(d, out);
  return out;
}

}  // namespace

void begin() {
  if (g_active) return;
  if (!g_mu) g_mu = xSemaphoreCreateMutex();

  // STA MAC straight from efuse — no WiFi.begin() required, so this is valid
  // even before the radio joins a network.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  String localName = makeLocalName(mac);
  String macStr    = makeMacString(mac);

  NimBLEDevice::init(std::string(localName.c_str()));
  // Honor the app's 247-byte MTU request so the credentials JSON lands in a
  // single ATT write.
  NimBLEDevice::setMTU(247);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());
  // We manage advertising explicitly from the net task (per the boot/setup
  // window + WiFi-connected rules), so disable NimBLE's auto-restart.
  g_server->advertiseOnDisconnect(false);

  NimBLEService* svc = g_server->createService(SVC_UUID);

  g_credChar = svc->createCharacteristic(
      CRED_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  g_credChar->setCallbacks(new CredCallbacks());

  // READ | NOTIFY makes NimBLE auto-create the standard 0x2902 CCCD.
  g_statusChar = svc->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  g_statusChar->setCallbacks(new StatusCallbacks());

  g_infoChar = svc->createCharacteristic(INFO_UUID, NIMBLE_PROPERTY::READ);
  String info = buildDeviceInfo(macStr);
  g_infoChar->setValue((uint8_t*)info.c_str(), info.length());

  // Seed the initial idle status so an early Read/subscribe sees a valid shape.
  g_statusJson = buildStatus("idle", "", "");
  g_statusChar->setValue((uint8_t*)g_statusJson.c_str(), g_statusJson.length());

  svc->start();

  // Advertise the 128-bit service UUID in the main packet (the app filters on
  // it) and put the local name in the scan response — a 128-bit UUID (16B) +
  // flags already nearly fills the 31-byte adv payload, so the name won't fit
  // alongside it.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.setCompleteServices(NimBLEUUID(SVC_UUID));
  adv->setAdvertisementData(advData);

  NimBLEAdvertisementData scanResp;
  scanResp.setName(std::string(localName.c_str()));
  adv->setScanResponseData(scanResp);
  adv->setScanResponse(true);

  g_active = true;
  log_i("[ble] provisioning server up: name='%s' mac=%s",
        localName.c_str(), macStr.c_str());

  startAdvertising();
}

bool active()          { return g_active; }
bool clientConnected() { return g_clientConn; }
bool advertising()     { return g_advertising; }

bool takePendingCreds(String& ssid, String& pass) {
  if (!g_active) return false;
  bool got = false;
  xSemaphoreTake(g_mu, portMAX_DELAY);
  if (g_pending) {
    ssid      = g_pendingSsid;
    pass      = g_pendingPass;
    g_pending = false;
    got       = true;
  }
  xSemaphoreGive(g_mu);
  return got;
}

void setStatus(const char* state, const String& ip, const char* reason) {
  if (!g_active || !g_statusChar) return;
  String json = buildStatus(state, ip, reason);
  xSemaphoreTake(g_mu, portMAX_DELAY);
  g_statusJson = json;
  xSemaphoreGive(g_mu);
  g_statusChar->setValue((uint8_t*)json.c_str(), json.length());
  g_statusChar->notify();
  log_i("[ble] status -> %s", json.c_str());
}

void startAdvertising() {
  if (!g_active || g_advertising || g_clientConn) return;
  NimBLEDevice::getAdvertising()->start();
  g_advertising = true;
  log_i("[ble] advertising started");
}

void stopAdvertising() {
  if (!g_active || !g_advertising) return;
  NimBLEDevice::getAdvertising()->stop();
  g_advertising = false;
  log_i("[ble] advertising stopped");
}

void end() {
  if (!g_active) return;
  log_i("[ble] tearing down BLE stack (reclaiming RAM)");
  NimBLEDevice::deinit(true);
  g_server     = nullptr;
  g_credChar   = nullptr;
  g_statusChar = nullptr;
  g_infoChar   = nullptr;
  g_active      = false;
  g_advertising = false;
  g_clientConn  = false;
}

}  // namespace ble_prov
