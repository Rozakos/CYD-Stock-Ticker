#include "settings_screen.h"

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "../config.h"
#include "../settings/settings_store.h"
#include "list_screen.h"
#include "styles.h"

namespace {

SettingsStore* g_settings = nullptr;
bool g_active = false;

lv_obj_t* g_scr     = nullptr;
lv_obj_t* g_ssid    = nullptr;
lv_obj_t* g_ip      = nullptr;
lv_obj_t* g_rssi    = nullptr;
lv_obj_t* g_refresh = nullptr;
lv_obj_t* g_syms    = nullptr;
lv_obj_t* g_key     = nullptr;
lv_obj_t* g_url     = nullptr;

void on_tap(lv_event_t*) {
  g_active = false;
  lv_screen_load(list_screen::screen());
}

// Caption/value row laid out as: [caption muted, fixed width] [value]
lv_obj_t* add_kv(lv_obj_t* parent, const char* caption) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* cap = lv_label_create(row);
  lv_obj_add_style(cap, &styles::muted, 0);
  lv_obj_set_width(cap, 64);
  lv_label_set_text(cap, caption);

  lv_obj_t* val = lv_label_create(row);
  lv_obj_add_style(val, &styles::price, 0);
  lv_obj_set_flex_grow(val, 1);
  lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
  lv_label_set_text(val, "—");
  return val;
}

void build_once() {
  if (g_scr) return;
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, styles::bg_color(), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_scr, 8, 0);
  // Flex column so header / cards / URL stack naturally without absolute
  // positioning — prior layout overflowed when the symbols row wrapped.
  lv_obj_set_flex_flow(g_scr, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(g_scr, 6, 0);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_scr, on_tap, LV_EVENT_CLICKED, nullptr);

  // Header
  lv_obj_t* header = lv_obj_create(g_scr);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, LV_PCT(100), 24);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(header, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* title = lv_label_create(header);
  lv_obj_add_style(title, &styles::sym_small, 0);
  lv_label_set_text(title, LV_SYMBOL_SETTINGS "  Settings");

  lv_obj_t* back = lv_label_create(header);
  lv_obj_add_style(back, &styles::muted, 0);
  lv_label_set_text(back, LV_SYMBOL_LEFT " tap");

  // Network card
  lv_obj_t* net = lv_obj_create(g_scr);
  lv_obj_remove_style_all(net);
  lv_obj_add_style(net, &styles::card, 0);
  lv_obj_set_size(net, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(net, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(net, 2, 0);
  lv_obj_clear_flag(net, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(net, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* net_title = lv_label_create(net);
  lv_obj_add_style(net_title, &styles::muted, 0);
  lv_label_set_text(net_title, "NETWORK");

  g_ssid = add_kv(net, "WiFi");
  g_ip   = add_kv(net, "IP");
  g_rssi = add_kv(net, "Signal");

  // Data card
  lv_obj_t* data = lv_obj_create(g_scr);
  lv_obj_remove_style_all(data);
  lv_obj_add_style(data, &styles::card, 0);
  lv_obj_set_size(data, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(data, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(data, 2, 0);
  lv_obj_clear_flag(data, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(data, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* data_title = lv_label_create(data);
  lv_obj_add_style(data_title, &styles::muted, 0);
  lv_label_set_text(data_title, "STOCK DATA");

  g_refresh = add_kv(data, "Refresh");
  g_syms    = add_kv(data, "Symbols");
  g_key     = add_kv(data, "API key");

  // Footer with web admin URL — sits naturally below the cards in flex flow.
  g_url = lv_label_create(g_scr);
  lv_obj_add_style(g_url, &styles::muted, 0);
  lv_label_set_long_mode(g_url, LV_LABEL_LONG_DOT);
  lv_obj_set_width(g_url, LV_PCT(100));
  lv_label_set_text(g_url, "Configure at http://<ip>/");
}

void refresh() {
  bool up = WiFi.status() == WL_CONNECTED;
  lv_label_set_text(g_ssid, up ? WiFi.SSID().c_str() : "not connected");
  lv_obj_set_style_text_color(
      g_ssid, up ? styles::up_color() : styles::dn_color(), 0);

  lv_label_set_text(g_ip, up ? WiFi.localIP().toString().c_str() : "—");

  char buf[64];
  if (up) {
    snprintf(buf, sizeof(buf), "%ld dBm", (long)WiFi.RSSI());
  } else {
    snprintf(buf, sizeof(buf), "—");
  }
  lv_label_set_text(g_rssi, buf);

  if (!g_settings) return;
  snprintf(buf, sizeof(buf), "%lus", (unsigned long)g_settings->refreshSeconds());
  lv_label_set_text(g_refresh, buf);

  auto syms = g_settings->symbols();
  String joined;
  for (size_t i = 0; i < syms.size(); ++i) {
    if (i) joined += ", ";
    joined += syms[i];
  }
  if (joined.length() == 0) joined = "—";
  lv_label_set_text(g_syms, joined.c_str());

  bool has_key = g_settings->apiKey().length() > 0;
  lv_label_set_text(g_key, has_key ? "set" : "not set");
  lv_obj_set_style_text_color(
      g_key, has_key ? styles::up_color() : styles::dn_color(), 0);

  if (up) {
    snprintf(buf, sizeof(buf), "Configure at http://%s/",
             WiFi.localIP().toString().c_str());
    lv_label_set_text(g_url, buf);
  } else {
    lv_label_set_text(g_url, "Configure once connected.");
  }
}

}  // namespace

namespace settings_screen {

void init(SettingsStore* settings) { g_settings = settings; }

void show() {
  build_once();
  refresh();
  g_active = true;
  lv_screen_load(g_scr);
}

void tick() {
  if (!g_active) return;
  refresh();
}

bool active() { return g_active; }

}  // namespace settings_screen
