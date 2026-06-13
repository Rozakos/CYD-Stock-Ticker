#include "list_screen.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "../config.h"
#include "../net/quote_store.h"
#include "../settings/settings_store.h"
#include "../util/area_fill.h"
#include "../util/interpolate.h"
#include "detail_screen.h"
#include "logos.h"
#include "settings_screen.h"
#include "styles.h"

namespace {

constexpr uint16_t STATUS_H     = 22;
constexpr uint16_t ROW_H        = 50;   // 4 * 50 + 3 * 4 = 212 px <= 218
constexpr uint16_t ROW_GAP      = 4;
constexpr uint16_t LOGO_SIZE    = 38;
constexpr uint16_t SPARK_W      = 96;
constexpr uint16_t SPARK_H      = 28;
constexpr uint16_t VISIBLE_ROWS = (cfg::SCREEN_H - STATUS_H) / (ROW_H + ROW_GAP);
// PCHIP smoothing factor for the sparkline. Smaller than the detail
// chart's factor (=5) because the row's plot area is only ~94 px wide;
// finer interpolation has nothing to render into.
constexpr int      SPARK_FACTOR  = 3;
constexpr int      SPARK_MAX_OUT =
    (cfg::SPARKLINE_POINTS - 1) * SPARK_FACTOR + 1;

QuoteStore*    g_store    = nullptr;
SettingsStore* g_settings = nullptr;

lv_obj_t*  g_scr        = nullptr;
lv_obj_t*  g_list       = nullptr;
lv_obj_t*  g_wifi_icon  = nullptr;
lv_obj_t*  g_updated    = nullptr;
lv_obj_t*  g_gear       = nullptr;
lv_timer_t* g_rot_timer = nullptr;

// Diameter of the crescent "after-hours" moon icon, in px.
constexpr lv_coord_t MOON_SIZE = 14;

struct Row {
  lv_obj_t* obj;
  lv_obj_t* logo;
  lv_obj_t* sym;
  // Crescent-moon badge under the ticker, shown only while the quote carries
  // an extended-hours (pre/post-market) price. Hidden during regular hours.
  lv_obj_t* after_icon = nullptr;
  lv_obj_t* price;
  lv_obj_t* pct;
  lv_obj_t* spark;
  // Sized for the PCHIP-smoothed output, not the raw close count.
  lv_point_precise_t spark_pts[SPARK_MAX_OUT];
  // Snapshot of the spark points in int32 for the polygon-fill helper.
  // Kept in lock-step with spark_pts; the lv_line widget owns the curve
  // and we own the fill underneath.
  int32_t    spark_xs[SPARK_MAX_OUT];
  int32_t    spark_ys[SPARK_MAX_OUT];
  int        spark_n     = 0;
  // Single source of truth for the row's accent colour. Both the lv_line
  // stroke and the area-fill draw event read this field, so they can't
  // drift apart through style-cascade quirks.
  lv_color_t accent      = lv_color_hex(0x8a98ad);
  // logos::signature() result for the logo currently mounted on this row.
  // rebuild_logo only fires when it changes, so embedded/badge rows skip
  // the widget churn every refresh tick.
  uint32_t   logo_sig    = 0;
  String     symbol;
};
std::vector<Row> g_rows;

// LV_EVENT_DRAW_MAIN_BEGIN handler on each sparkline. Draws the area
// gradient (line colour at top → transparent at bottom) before the
// lv_line widget renders the stroke on top.
void spark_fill_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_BEGIN) return;
  lv_obj_t* spark = (lv_obj_t*)lv_event_get_target(e);
  if (!spark) return;
  lv_layer_t* layer = lv_event_get_layer(e);
  if (!layer) return;

  size_t idx = (size_t)(uintptr_t)lv_obj_get_user_data(spark);
  if (idx >= g_rows.size()) return;
  const Row& r = g_rows[idx];
  if (r.spark_n < 2) return;

  lv_area_t coords;
  lv_obj_get_coords(spark, &coords);
  // Bottom of the polygon: just inside the obj's bottom edge so the
  // gradient runs the full sparkline height. SPARK_H - 1 matches what
  // the inclusive lv_area_t expects. Colour comes from the row's
  // `accent` field — same value used for the lv_line stroke, so the
  // fill base colour cannot drift from the line colour.
  util::draw_polyline_fill(layer, r.spark_xs, r.spark_ys, r.spark_n,
                           coords.x1, coords.y1,
                           SPARK_H - 1, r.accent, LV_OPA_50);
}

void open_detail_async(void* user_data) {
  const char* sym = static_cast<const char*>(user_data);
  if (!sym) return;
  detail_screen::show(g_store, sym);
}

// Per-row long-press has to suppress the click that would otherwise fire
// on release (LVGL fires both LONG_PRESSED and CLICKED for the same gesture
// by default). The static flag is set when LONG_PRESSED fires and consumed
// by the next CLICKED on the same row, then reset.
bool g_swallow_next_click = false;
// Set by the long-press handler — tick() picks this up and triggers the
// reorder rebuild. We don't clean the list from inside the event handler
// because LVGL would use-after-free the obj on dispatch return.
bool g_favourites_dirty = false;

void on_row_click(lv_event_t* e) {
  if (g_swallow_next_click) {
    g_swallow_next_click = false;
    return;
  }
  const char* sym = static_cast<const char*>(lv_event_get_user_data(e));
  if (!sym) return;
  lv_async_call(open_detail_async, (void*)sym);
}

void on_row_long_press(lv_event_t* e) {
  const char* sym = static_cast<const char*>(lv_event_get_user_data(e));
  if (!sym || !g_settings) return;
  g_settings->toggleFavourite(sym);
  g_swallow_next_click = true;
  g_favourites_dirty   = true;
}

// The row's heap-allocated symbol string (shared by the click + long-press
// callbacks) is owned by us, not LVGL. Free it when the row obj is destroyed
// (lv_obj_clean / delete on every reorder or symbol-set change) so it doesn't
// leak one string per row on each rebuild.
void on_row_delete(lv_event_t* e) {
  free(lv_event_get_user_data(e));
}

// Builds a small crescent-moon "extended hours" badge as a child of `parent`.
// LVGL's Montserrat font ships no moon glyph, so the crescent is drawn from
// primitives: a filled disc in `moonColor` with a slightly offset disc in the
// row background colour stacked on top, carving out the right side. The cutout
// colour matches the row card so the crescent reads cleanly against it.
lv_obj_t* make_moon(lv_obj_t* parent) {
  lv_obj_t* cont = lv_obj_create(parent);
  lv_obj_remove_style_all(cont);
  lv_obj_set_size(cont, MOON_SIZE, MOON_SIZE);
  lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(cont, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* disc = lv_obj_create(cont);
  lv_obj_remove_style_all(disc);
  lv_obj_set_size(disc, MOON_SIZE, MOON_SIZE);
  lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(disc, lv_color_hex(0xcbd5e1), 0);  // pale moonlight
  lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
  lv_obj_align(disc, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(disc, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Cutout disc, same size, offset up-and-right; rendered after the body disc
  // so it paints over it. Background colour == row card so it dissolves into
  // the row, leaving a crescent opening toward the lower-left.
  lv_obj_t* cut = lv_obj_create(cont);
  lv_obj_remove_style_all(cut);
  lv_obj_set_size(cut, MOON_SIZE, MOON_SIZE);
  lv_obj_set_style_radius(cut, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(cut, styles::card_color(), 0);
  lv_obj_set_style_bg_opa(cut, LV_OPA_COVER, 0);
  lv_obj_align(cut, LV_ALIGN_CENTER, 5, -2);
  lv_obj_add_flag(cut, LV_OBJ_FLAG_EVENT_BUBBLE);
  return cont;
}

Row make_row(lv_obj_t* parent, const String& symbol) {
  Row r{};
  r.symbol = symbol;

  r.obj = lv_obj_create(parent);
  lv_obj_remove_style_all(r.obj);
  lv_obj_add_style(r.obj, &styles::row, LV_STATE_DEFAULT);
  lv_obj_add_style(r.obj, &styles::row_pressed, LV_STATE_PRESSED);
  lv_obj_set_size(r.obj, cfg::SCREEN_W - 12, ROW_H);
  lv_obj_set_flex_flow(r.obj, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r.obj, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r.obj, 6, 0);
  lv_obj_set_scrollbar_mode(r.obj, LV_SCROLLBAR_MODE_OFF);
  lv_obj_clear_flag(r.obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);

  // Store the signature of what was ACTUALLY mounted (not signature(), which
  // reports what's available): if a runtime decode was deferred and a badge
  // went up instead, the recorded badge sig stays != signature() so rebuild_logo
  // retries on a later refresh rather than assuming the row is up to date.
  r.logo     = logos::make(r.obj, symbol, LOGO_SIZE, &r.logo_sig);
  lv_obj_add_flag(r.logo, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* sym_col = lv_obj_create(r.obj);
  lv_obj_remove_style_all(sym_col);
  lv_obj_set_size(sym_col, 60, ROW_H - 8);
  lv_obj_set_flex_flow(sym_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sym_col, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(sym_col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(sym_col, LV_OBJ_FLAG_EVENT_BUBBLE);

  r.sym = lv_label_create(sym_col);
  lv_obj_add_style(r.sym, &styles::sym_small, 0);
  // "* " prefix marks favourites — Montserrat ships with the ASCII
  // asterisk, no font rebuild needed.
  if (g_settings && g_settings->isFavourite(symbol)) {
    String marked = "* " + symbol;
    lv_label_set_text(r.sym, marked.c_str());
  } else {
    lv_label_set_text(r.sym, symbol.c_str());
  }

  // After-hours moon, stacked under the ticker. Hidden until rebuild_rows sees
  // an extended-hours price for this symbol.
  r.after_icon = make_moon(sym_col);
  lv_obj_add_flag(r.after_icon, LV_OBJ_FLAG_HIDDEN);

  r.spark = lv_line_create(r.obj);
  lv_obj_set_size(r.spark, SPARK_W, SPARK_H);
  lv_obj_set_style_line_width(r.spark, 2, 0);
  lv_obj_set_style_line_rounded(r.spark, true, 0);
  lv_obj_set_style_line_color(r.spark, styles::muted_color(), 0);
  lv_obj_add_flag(r.spark, LV_OBJ_FLAG_EVENT_BUBBLE);
  // Row index baked into user_data so the polygon-fill draw event can
  // look this Row up in g_rows. Wired up after push_back below — the
  // make_row call returns a temp Row, the final address only stabilises
  // once it's in g_rows (which we reserve() so push_back doesn't move).
  lv_obj_add_event_cb(r.spark, spark_fill_cb,
                      LV_EVENT_DRAW_MAIN_BEGIN, nullptr);

  lv_obj_t* price_col = lv_obj_create(r.obj);
  lv_obj_remove_style_all(price_col);
  lv_obj_set_size(price_col, LV_SIZE_CONTENT, ROW_H - 8);
  lv_obj_set_flex_grow(price_col, 1);
  lv_obj_set_flex_flow(price_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(price_col, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_clear_flag(price_col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(price_col, LV_OBJ_FLAG_EVENT_BUBBLE);

  r.price = lv_label_create(price_col);
  lv_obj_add_style(r.price, &styles::price, 0);
  lv_label_set_text(r.price, "—");

  r.pct = lv_label_create(price_col);
  lv_obj_add_style(r.pct, &styles::muted, 0);
  lv_label_set_text(r.pct, "—");

  // heapSym lives forever — the obj retains it for the click handler and
  // the long-press handler (both fire on the same row).
  char* heapSym = strdup(symbol.c_str());
  lv_obj_add_event_cb(r.obj, on_row_click,      LV_EVENT_CLICKED,      heapSym);
  lv_obj_add_event_cb(r.obj, on_row_long_press, LV_EVENT_LONG_PRESSED, heapSym);
  lv_obj_add_event_cb(r.obj, on_row_delete,     LV_EVENT_DELETE,       heapSym);
  return r;
}

// True while some row's mounted logo doesn't match the best available source
// (typically a badge standing in for a deferred PNG decode). Drives the
// retry sweep in tick() so a deferred logo heals within seconds instead of
// waiting for the next quote refresh — by then the net task has re-opened
// its TLS session and the decode would be starved all over again.
bool g_logo_retry_pending = false;

// Returns true when the mounted widget now matches what logos::make would
// produce (row settled), false when a retry is still pending.
bool rebuild_logo(Row& r) {
  // No-op when the logo source hasn't changed since the last build —
  // embedded logos always return the same signature and the badge
  // fallback never changes either. Only newly-cached runtime PNGs or
  // a swap between source kinds force the widget recreation.
  uint32_t want = logos::signature(r.symbol);
  if (r.logo && want == r.logo_sig) return true;
  if (r.logo) lv_obj_delete(r.logo);
  // Record what was actually mounted, not `want`: a deferred/failed runtime
  // decode mounts a badge, leaving logo_sig != want so a later pass retries.
  r.logo     = logos::make(r.obj, r.symbol, LOGO_SIZE, &r.logo_sig);
  lv_obj_add_flag(r.logo, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_move_to_index(r.logo, 0);
  return r.logo_sig == want;
}

void update_spark(Row& r, const std::vector<float>& closes, bool up) {
  if (closes.size() < 2) {
    r.spark_n = 0;
    lv_line_set_points(r.spark, r.spark_pts, 0);
    lv_obj_invalidate(r.spark);
    return;
  }
  size_t n_in = closes.size();
  if (n_in > cfg::SPARKLINE_POINTS) n_in = cfg::SPARKLINE_POINTS;
  float lo = closes.back(), hi = closes.back();
  for (size_t i = closes.size() - n_in; i < closes.size(); ++i) {
    if (closes[i] < lo) lo = closes[i];
    if (closes[i] > hi) hi = closes[i];
  }
  float span = hi - lo;
  if (span < 0.0001f) span = 1.0f;

  // PCHIP smooth into a static buffer — same monotone cubic Hermite that
  // the detail chart uses, so the two curves are visually consistent.
  // factor=3 is enough for the ~94 px sparkline plot width.
  int out_n = (int)(n_in - 1) * SPARK_FACTOR + 1;
  if (out_n > SPARK_MAX_OUT) out_n = SPARK_MAX_OUT;
  static float smooth[SPARK_MAX_OUT];
  util::monotone_cubic_interpolate(&closes[closes.size() - n_in], n_in,
                                   smooth, (size_t)out_n, SPARK_FACTOR);

  const lv_coord_t W = SPARK_W - 2;
  const lv_coord_t H = SPARK_H - 4;
  for (int i = 0; i < out_n; ++i) {
    float v = smooth[i];
    float t = (out_n == 1) ? 0.5f : (float)i / (float)(out_n - 1);
    float x = 1.0f + t * (float)W;
    float y = 2.0f + (1.0f - (v - lo) / span) * (float)H;
    r.spark_pts[i].x = (lv_value_precise_t)x;
    r.spark_pts[i].y = (lv_value_precise_t)y;
    r.spark_xs[i] = (int32_t)(x + 0.5f);
    r.spark_ys[i] = (int32_t)(y + 0.5f);
  }
  r.spark_n = out_n;
  // Both the stroke and the area-fill source their colour from r.accent —
  // matches the same accent the +%/-% label arrows use (styles::pct_up /
  // pct_dn via up_color() / dn_color()).
  r.accent = up ? styles::up_color() : styles::dn_color();
  lv_line_set_points(r.spark, r.spark_pts, out_n);
  lv_obj_set_style_line_color(r.spark, r.accent, 0);
  lv_obj_invalidate(r.spark);
}

void rebuild_rows(const std::vector<Quote>& quotes_in) {
  // Stable-partition favourites to the front, preserving relative order in
  // both halves so the rest of the list stays in its natural API order.
  std::vector<Quote> quotes;
  quotes.reserve(quotes_in.size());
  if (g_settings) {
    for (const auto& q : quotes_in) {
      if (g_settings->isFavourite(q.symbol)) quotes.push_back(q);
    }
    for (const auto& q : quotes_in) {
      if (!g_settings->isFavourite(q.symbol)) quotes.push_back(q);
    }
  } else {
    quotes = quotes_in;
  }

  bool same = quotes.size() == g_rows.size();
  for (size_t i = 0; same && i < quotes.size(); ++i) {
    if (quotes[i].symbol != g_rows[i].symbol) same = false;
  }
  if (!same) {
    lv_obj_clean(g_list);
    g_rows.clear();
    logos::clearRuntimeCache();
    g_rows.reserve(quotes.size());
    for (const auto& q : quotes) g_rows.push_back(make_row(g_list, q.symbol));
    // Bake the row index into each sparkline's user_data now that the
    // Row's address is stable (g_rows was reserve()'d above, so push_back
    // doesn't relocate). spark_fill_cb uses this to look up the row.
    for (size_t i = 0; i < g_rows.size(); ++i) {
      lv_obj_set_user_data(g_rows[i].spark, (void*)(uintptr_t)i);
    }
    // make_row may have mounted badges for deferred decodes; let the first
    // retry sweep verify (it clears the flag once every row settles).
    g_logo_retry_pending = true;
  } else {
    // A quote refresh can download a missing runtime logo into LittleFS
    // without changing the symbol list. Rebuild just the logo widget so a
    // row that started as a badge can switch to /logos/<SYMBOL>.png.
    g_logo_retry_pending = false;
    for (auto& r : g_rows) {
      if (!rebuild_logo(r)) g_logo_retry_pending = true;
    }
  }
  for (size_t i = 0; i < quotes.size(); ++i) {
    const auto& q = quotes[i];
    Row& r = g_rows[i];
    if (!q.fresh) {
      lv_label_set_text(r.price, "—");
      lv_label_set_text(r.pct, "—");
      lv_obj_remove_style(r.pct, &styles::pct_up, 0);
      lv_obj_remove_style(r.pct, &styles::pct_dn, 0);
      lv_obj_add_style(r.pct, &styles::muted, 0);
      lv_line_set_points(r.spark, r.spark_pts, 0);
      if (r.after_icon) lv_obj_add_flag(r.after_icon, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    // During extended hours the row headlines the pre/post-market print (the
    // way Revolut does), flagged by the moon icon under the ticker. Falls back
    // to the regular last/change when no extended price is available.
    bool  ext      = q.extended();
    float showPrice = ext ? q.extPrice     : q.last;
    float showPct   = ext ? q.extChangePct : q.changePct;
    if (isnan(showPct)) showPct = 0.0f;
    bool  up        = showPct >= 0;

    if (r.after_icon) {
      if (ext) lv_obj_remove_flag(r.after_icon, LV_OBJ_FLAG_HIDDEN);
      else     lv_obj_add_flag   (r.after_icon, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f", showPrice);
    lv_label_set_text(r.price, buf);
    snprintf(buf, sizeof(buf), "%s %+.2f%%",
             up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, showPct);
    lv_label_set_text(r.pct, buf);

    lv_obj_remove_style(r.pct, &styles::muted, 0);
    lv_obj_remove_style(r.pct, &styles::pct_up, 0);
    lv_obj_remove_style(r.pct, &styles::pct_dn, 0);
    lv_obj_add_style(r.pct, up ? &styles::pct_up : &styles::pct_dn, 0);
    update_spark(r, q.sparkline, up);
  }
}

void refresh_wifi() {
  if (WiFi.status() == WL_CONNECTED) {
    // Just the signal glyph when connected — the dBm number was noise.
    lv_label_set_text(g_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(g_wifi_icon, styles::up_color(), 0);
  } else {
    lv_label_set_text(g_wifi_icon, LV_SYMBOL_CLOSE " no link");
    lv_obj_set_style_text_color(g_wifi_icon, styles::dn_color(), 0);
  }
}

void update_status(time_t lastUpdate) {
  refresh_wifi();
  if (lastUpdate == 0) {
    lv_label_set_text(g_updated, "—");
  } else {
    struct tm t;
    localtime_r(&lastUpdate, &t);
    char buf[24];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             t.tm_hour, t.tm_min, t.tm_sec);
    lv_label_set_text(g_updated, buf);
  }
}

void on_gear_click(lv_event_t*) {
  settings_screen::show();
}

void on_wifi_reset_confirm(lv_event_t* e) {
  SettingsStore* s = static_cast<SettingsStore*>(lv_event_get_user_data(e));
  if (s) s->setWifi("", "");
  delay(150);          // give LittleFS time to flush
  ESP.restart();
}

void on_wifi_reset_cancel(lv_event_t* e) {
  auto* btn  = static_cast<lv_obj_t*>(lv_event_get_target(e));
  auto* mbox = lv_obj_get_parent(lv_obj_get_parent(btn));   // btn -> footer -> mbox
  lv_msgbox_close(mbox);
}

void on_wifi_click(lv_event_t* e) {
  SettingsStore* s = static_cast<SettingsStore*>(lv_event_get_user_data(e));
  lv_obj_t* mbox = lv_msgbox_create(nullptr);
  lv_msgbox_add_title(mbox, "Reset WiFi");
  lv_msgbox_add_text(mbox,
      "Forget the saved network and reboot into setup mode (QR code)?");
  lv_obj_t* cancel = lv_msgbox_add_footer_button(mbox, "Cancel");
  lv_obj_add_event_cb(cancel, on_wifi_reset_cancel, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* reset = lv_msgbox_add_footer_button(mbox, "Reset");
  lv_obj_add_event_cb(reset, on_wifi_reset_confirm, LV_EVENT_CLICKED, s);
}

void rotate_cb(lv_timer_t*) {
  if (g_rows.size() <= VISIBLE_ROWS) return;
  lv_coord_t y = lv_obj_get_scroll_y(g_list);
  lv_coord_t max = lv_obj_get_scroll_bottom(g_list) + y;
  lv_coord_t next = y + (ROW_H + ROW_GAP);
  if (next >= max) next = 0;
  lv_obj_scroll_to_y(g_list, next, LV_ANIM_ON);
}

}  // namespace

namespace list_screen {

void build(QuoteStore* store, SettingsStore* settings) {
  g_store    = store;
  g_settings = settings;
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, styles::bg_color(), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_scr, 0, 0);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* bar = lv_obj_create(g_scr);
  lv_obj_remove_style_all(bar);
  lv_obj_add_style(bar, &styles::status_bar, 0);
  lv_obj_set_size(bar, cfg::SCREEN_W, STATUS_H);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  g_wifi_icon = lv_label_create(bar);
  lv_obj_add_style(g_wifi_icon, &styles::status_text, 0);
  lv_label_set_text(g_wifi_icon, LV_SYMBOL_CLOSE " no link");
  lv_obj_set_style_text_color(g_wifi_icon, styles::dn_color(), 0);
  // Tap to forget WiFi + reboot into setup-AP (QR onboarding).
  lv_obj_set_style_pad_hor(g_wifi_icon, 6, 0);
  lv_obj_set_style_pad_ver(g_wifi_icon, 2, 0);
  lv_obj_add_flag(g_wifi_icon, LV_OBJ_FLAG_CLICKABLE);
  // Expand the hit-box well beyond the small label so the tap is reachable
  // on a 320x240 panel — touch precision is poor and the status bar is 22 px.
  lv_obj_set_ext_click_area(g_wifi_icon, 14);
  lv_obj_add_event_cb(g_wifi_icon, on_wifi_click, LV_EVENT_CLICKED, g_settings);

  lv_obj_t* title = lv_label_create(bar);
  lv_obj_add_style(title, &styles::status_text, 0);
  lv_label_set_text(title, "MARKETS");

  g_updated = lv_label_create(bar);
  lv_obj_add_style(g_updated, &styles::status_text, 0);
  lv_label_set_text(g_updated, "—");

  g_gear = lv_label_create(bar);
  lv_obj_add_style(g_gear, &styles::status_text, 0);
  lv_label_set_text(g_gear, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_pad_hor(g_gear, 6, 0);
  lv_obj_set_style_pad_ver(g_gear, 2, 0);
  lv_obj_add_flag(g_gear, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(g_gear, 14);
  lv_obj_add_event_cb(g_gear, on_gear_click, LV_EVENT_CLICKED, nullptr);

  g_list = lv_obj_create(g_scr);
  lv_obj_remove_style_all(g_list);
  lv_obj_set_size(g_list, cfg::SCREEN_W, cfg::SCREEN_H - STATUS_H);
  lv_obj_align(g_list, LV_ALIGN_TOP_MID, 0, STATUS_H);
  lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(g_list, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(g_list, ROW_GAP, 0);
  lv_obj_set_style_pad_ver(g_list, ROW_GAP, 0);
  lv_obj_set_style_bg_color(g_list, styles::bg_color(), 0);
  lv_obj_set_style_bg_opa(g_list, LV_OPA_COVER, 0);
  lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(g_list, LV_SCROLLBAR_MODE_OFF);

  g_rot_timer = lv_timer_create(rotate_cb, 4000, nullptr);
}

lv_obj_t* screen() { return g_scr; }

void tick() {
  if (!g_store) return;
  static time_t lastSeen = 0;
  auto quotes = g_store->snapshot();
  time_t lu = g_store->lastUpdate();
  if (g_favourites_dirty) {
    // Wipe rows so the next rebuild_rows call rebuilds in the new order
    // (the symbol set is the same — just the partition order changed).
    lv_obj_clean(g_list);
    g_rows.clear();
    logos::clearRuntimeCache();
    g_favourites_dirty = false;
  }
  if (lu != lastSeen || g_rows.size() != quotes.size()) {
    rebuild_rows(quotes);
    update_status(lu);
    lastSeen = lu;
  } else {
    refresh_wifi();
  }

  // Retry sweep for logos whose decode was deferred under heap pressure.
  // When that happens the net task drops its TLS session (see netTask), so
  // the contiguous block recovers within a second or two — retry here rather
  // than waiting a whole refresh cycle, by which point the next fetch has
  // re-opened TLS and the decode would be starved again. rebuild_logo is a
  // cheap signature probe per row when nothing changed.
  static uint32_t lastLogoRetry = 0;
  if (g_logo_retry_pending && millis() - lastLogoRetry > 3000) {
    lastLogoRetry = millis();
    bool pending = false;
    for (auto& r : g_rows) {
      if (!rebuild_logo(r)) pending = true;
    }
    g_logo_retry_pending = pending;
  }
}

}  // namespace list_screen
