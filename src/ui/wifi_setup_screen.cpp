#include "wifi_setup_screen.h"

#include <lvgl.h>

#include "../config.h"
#include "../net/wifi_mgr.h"
#include "styles.h"

namespace wifi_setup_screen {

namespace {

lv_obj_t* g_scr      = nullptr;
lv_obj_t* g_qr       = nullptr;
lv_obj_t* g_ssid_lbl = nullptr;
lv_obj_t* g_status   = nullptr;
bool      g_active   = false;

// WIFI:T:<WPA|nopass>;S:<ssid>;P:<pass>;;
String wifiPayload(const String& ssid, const String& pass) {
  String type = pass.length() ? "WPA" : "nopass";
  String p;
  p.reserve(48 + ssid.length() + pass.length());
  p += "WIFI:T:";
  p += type;
  p += ";S:";
  p += ssid;
  p += ";P:";
  p += pass;
  p += ";;";
  return p;
}

void build_once(const String& ap_ssid, const String& ap_pass) {
  if (g_scr) return;
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, styles::bg_color(), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_scr, 8, 0);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(g_scr, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_scr, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(g_scr, 10, 0);

  // QR — sized to fit the 240 px height with a little margin.
  const int qr_size = 180;
  g_qr = lv_qrcode_create(g_scr);
  lv_qrcode_set_size(g_qr, qr_size);
  lv_qrcode_set_dark_color(g_qr, lv_color_black());
  lv_qrcode_set_light_color(g_qr, lv_color_white());
  String payload = wifiPayload(ap_ssid, ap_pass);
  lv_qrcode_update(g_qr, payload.c_str(), payload.length());

  // Right column: instructions.
  lv_obj_t* col = lv_obj_create(g_scr);
  lv_obj_remove_style_all(col);
  lv_obj_set_flex_grow(col, 1);
  lv_obj_set_height(col, LV_PCT(100));
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(col, 6, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(col);
  lv_obj_add_style(title, &styles::sym_small, 0);
  lv_label_set_text(title, "WiFi setup");

  lv_obj_t* hint = lv_label_create(col);
  lv_obj_add_style(hint, &styles::muted, 0);
  lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(hint, LV_PCT(100));
  lv_label_set_text(hint, "Scan the QR or join:");

  g_ssid_lbl = lv_label_create(col);
  lv_obj_add_style(g_ssid_lbl, &styles::price, 0);
  lv_obj_set_width(g_ssid_lbl, LV_PCT(100));
  lv_label_set_long_mode(g_ssid_lbl, LV_LABEL_LONG_WRAP);
  lv_label_set_text(g_ssid_lbl, ap_ssid.c_str());

  lv_obj_t* url = lv_label_create(col);
  lv_obj_add_style(url, &styles::muted, 0);
  lv_obj_set_width(url, LV_PCT(100));
  lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
  lv_label_set_text(url, "then open\nhttp://192.168.4.1/");

  g_status = lv_label_create(col);
  lv_obj_add_style(g_status, &styles::muted, 0);
  lv_obj_set_width(g_status, LV_PCT(100));
  lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
  lv_label_set_text(g_status, "waiting for setup…");
}

}  // namespace

void show(const String& ap_ssid, const String& ap_pass) {
  build_once(ap_ssid, ap_pass);
  g_active = true;
  lv_screen_load(g_scr);
}

void tick() {
  if (!g_active || !g_status) return;
  if (wifi_mgr::connected()) {
    lv_label_set_text_fmt(g_status, "connected — %s",
                          wifi_mgr::ip().c_str());
    lv_obj_set_style_text_color(g_status, styles::up_color(), 0);
  } else if (wifi_mgr::apActive()) {
    lv_label_set_text(g_status, "waiting for setup…");
    lv_obj_set_style_text_color(g_status, styles::muted_color(), 0);
  }
}

}  // namespace wifi_setup_screen
