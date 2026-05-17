#include "detail_screen.h"

#include <math.h>
#include <time.h>

#include "../config.h"
#include "../util/interpolate.h"
#include "../net/quote_store.h"
#include "list_screen.h"
#include "logos.h"
#include "styles.h"

namespace {

static constexpr int CARD_H      = cfg::SCREEN_H - 54 - 24 - 4;  // 158
static constexpr int CHART_W     = cfg::SCREEN_W - 32;            // 288
static constexpr int CHART_H     = CARD_H - 16;                   // 142
static constexpr int Y_DIV_CNT   = 5;
static constexpr int MAX_Y_TICKS = 8;     // pool — actual count chosen by step picker
static constexpr int X_TICK_COUNT = 3;    // visible X ticks; rightmost dropped to clear marker
static constexpr int CR_FACTOR    = 5;
static constexpr int CR_MAX_OUT   = (cfg::HISTORY_POINTS - 1) * CR_FACTOR + 1;
static constexpr int MARKER_DOT_SIZE = 8;

// Month abbreviations — fixed list so we avoid locale-dependent strftime.
// Latin only: LVGL's bundled Montserrat fonts don't include Greek glyphs.
static const char* kMonths[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

// Step picker for "nice" Y-axis tick spacing. Pick the smallest step S such
// that the data range divided by S yields at most ~5 ticks, then snap data
// min/max outwards to the nearest multiple of S before drawing labels.
static const float kSteps[] = {
    0.5f, 1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000, 2000, 5000
};

QuoteStore* g_store    = nullptr;
String      g_symbol;
bool        g_active   = false;
bool        g_rendered = false;

lv_obj_t* g_scr        = nullptr;
lv_obj_t* g_header     = nullptr;
lv_obj_t* g_logo_slot  = nullptr;
lv_obj_t* g_title      = nullptr;
lv_obj_t* g_back_hint  = nullptr;
lv_obj_t* g_price      = nullptr;
lv_obj_t* g_change     = nullptr;
lv_obj_t* g_chart      = nullptr;
lv_chart_series_t* g_ser = nullptr;
lv_obj_t* g_spinner    = nullptr;
lv_obj_t* g_y_labels[MAX_Y_TICKS]  = {};
lv_obj_t* g_x_labels[X_TICK_COUNT] = {};
lv_obj_t* g_marker_dot   = nullptr;
lv_obj_t* g_marker_label = nullptr;
lv_color_t g_line_color  = lv_color_hex(0x4ade80);   // updated per render

// Pre-computed line geometry handed to the draw callback so we never need
// to call lv_chart_get_point_pos_by_id from inside DRAW_MAIN_BEGIN (it
// reaches into chart-internal state that is mid-flight during draw).
int     g_fill_n          = 0;
int32_t g_fill_x[CR_MAX_OUT];
int32_t g_fill_y[CR_MAX_OUT];
int32_t g_fill_bottom_y   = 0;

void on_tap(lv_event_t*) {
  g_active = false;
  lv_screen_load(list_screen::screen());
}

// LVGL 9 line charts don't have a built-in area fill style — bg styles on
// LV_PART_ITEMS only paint the line itself, leaving the area below it
// untouched. We draw the fill manually here as a strip of vertical-gradient
// trapezoids (split into pairs of triangles) under each line segment. The
// handler is wired on LV_EVENT_DRAW_MAIN_BEGIN so it lands before the chart
// class renders the line on top of our fill.
void chart_area_fill_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_BEGIN) return;
  if (g_fill_n < 2) return;

  lv_obj_t* chart = (lv_obj_t*)lv_event_get_target(e);
  if (!chart) return;

  lv_layer_t* layer = lv_event_get_layer(e);
  if (!layer) return;

  lv_area_t obj_coords;
  lv_obj_get_coords(chart, &obj_coords);
  const int32_t cx = obj_coords.x1;
  const int32_t cy = obj_coords.y1;
  const int32_t ay_bot = cy + g_fill_bottom_y;

  lv_draw_triangle_dsc_t tdsc;
  lv_draw_triangle_dsc_init(&tdsc);
  tdsc.color = g_line_color;
  tdsc.opa   = LV_OPA_COVER;
  tdsc.grad.dir         = LV_GRAD_DIR_VER;
  tdsc.grad.stops_count = 2;
  tdsc.grad.stops[0].color = g_line_color;
  tdsc.grad.stops[0].opa   = LV_OPA_60;
  tdsc.grad.stops[0].frac  = 0;
  tdsc.grad.stops[1].color = g_line_color;
  tdsc.grad.stops[1].opa   = LV_OPA_TRANSP;
  tdsc.grad.stops[1].frac  = 255;

  // Sample every Nth point so we issue ~50 trapezoids (≈100 triangles)
  // rather than ~150. Visually indistinguishable; cheaper on the ESP32.
  const int stride = (g_fill_n > 60) ? (g_fill_n / 50) : 1;

  for (int i = 0; i + stride < g_fill_n; i += stride) {
    int32_t ax0 = cx + g_fill_x[i],          ay0 = cy + g_fill_y[i];
    int32_t ax1 = cx + g_fill_x[i + stride], ay1 = cy + g_fill_y[i + stride];

    tdsc.p[0].x = ax0; tdsc.p[0].y = ay0;
    tdsc.p[1].x = ax1; tdsc.p[1].y = ay1;
    tdsc.p[2].x = ax1; tdsc.p[2].y = ay_bot;
    lv_draw_triangle(layer, &tdsc);

    tdsc.p[0].x = ax0; tdsc.p[0].y = ay0;
    tdsc.p[1].x = ax1; tdsc.p[1].y = ay_bot;
    tdsc.p[2].x = ax0; tdsc.p[2].y = ay_bot;
    lv_draw_triangle(layer, &tdsc);
  }
}

void build_once() {
  if (g_scr) return;
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, styles::bg_color(), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_scr, 8, 0);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(g_scr, on_tap, LV_EVENT_CLICKED, nullptr);

  g_header = lv_obj_create(g_scr);
  lv_obj_remove_style_all(g_header);
  lv_obj_set_size(g_header, cfg::SCREEN_W - 16, 54);
  lv_obj_align(g_header, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_flex_flow(g_header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_header, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(g_header, 8, 0);
  lv_obj_clear_flag(g_header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_header, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_logo_slot = lv_obj_create(g_header);
  lv_obj_remove_style_all(g_logo_slot);
  lv_obj_set_size(g_logo_slot, 48, 48);
  lv_obj_clear_flag(g_logo_slot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_logo_slot, LV_OBJ_FLAG_EVENT_BUBBLE);

  lv_obj_t* title_col = lv_obj_create(g_header);
  lv_obj_remove_style_all(title_col);
  lv_obj_set_size(title_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(title_col, 1);
  lv_obj_set_flex_flow(title_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(title_col, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(title_col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(title_col, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_title = lv_label_create(title_col);
  lv_obj_add_style(g_title, &styles::sym, 0);

  g_price = lv_label_create(title_col);
  lv_obj_add_style(g_price, &styles::price_big, 0);
  lv_label_set_text(g_price, "—");

  lv_obj_t* right_col = lv_obj_create(g_header);
  lv_obj_remove_style_all(right_col);
  lv_obj_set_size(right_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right_col, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_clear_flag(right_col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(right_col, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_back_hint = lv_label_create(right_col);
  lv_obj_add_style(g_back_hint, &styles::muted, 0);
  lv_label_set_text(g_back_hint, LV_SYMBOL_LEFT " tap");

  g_change = lv_label_create(right_col);
  lv_obj_add_style(g_change, &styles::price, 0);
  lv_label_set_text(g_change, "loading…");

  lv_obj_t* card = lv_obj_create(g_scr);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &styles::card, 0);
  lv_obj_set_size(card, cfg::SCREEN_W - 16, CARD_H);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 56);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_chart = lv_chart_create(card);
  lv_obj_set_size(g_chart, CHART_W, CHART_H);
  lv_obj_align(g_chart, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(g_chart, cfg::HISTORY_POINTS);
  lv_chart_set_div_line_count(g_chart, Y_DIV_CNT, 0);
  lv_chart_set_update_mode(g_chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_obj_set_style_pad_all(g_chart, 0, LV_PART_MAIN);
  lv_obj_set_style_size(g_chart, 0, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(g_chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(g_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_chart, 0, 0);
  lv_obj_set_style_line_color(g_chart, lv_color_hex(0x2a3548), LV_PART_MAIN);
  lv_obj_set_style_line_opa(g_chart, LV_OPA_30, LV_PART_MAIN);
  lv_obj_add_flag(g_chart, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(g_chart, chart_area_fill_cb,
                      LV_EVENT_DRAW_MAIN_BEGIN, nullptr);

  g_ser = lv_chart_add_series(g_chart, styles::up_color(),
                              LV_CHART_AXIS_PRIMARY_Y);

  g_spinner = lv_spinner_create(card);
  lv_obj_set_size(g_spinner, 36, 36);
  lv_obj_center(g_spinner);

  // Y-label pool — created hidden; render_history shows the ones it needs.
  for (int j = 0; j < MAX_Y_TICKS; ++j) {
    lv_obj_t* lbl = lv_label_create(g_chart);
    lv_obj_add_style(lbl, &styles::muted, 0);
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    g_y_labels[j] = lbl;
  }

  // X-axis date labels — 3 of them, positions computed per-render so they
  // line up with the tick indices [0, n/3, 2n/3]. The rightmost tick (n-1)
  // is intentionally skipped to leave clean space for the current-price
  // marker that always lands at the line tip.
  for (int k = 0; k < X_TICK_COUNT; ++k) {
    lv_obj_t* lbl = lv_label_create(g_chart);
    lv_obj_add_style(lbl, &styles::muted, 0);
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    g_x_labels[k] = lbl;
  }

  g_marker_dot = lv_obj_create(g_chart);
  lv_obj_remove_style_all(g_marker_dot);
  lv_obj_set_size(g_marker_dot, MARKER_DOT_SIZE, MARKER_DOT_SIZE);
  lv_obj_set_style_radius(g_marker_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(g_marker_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(g_marker_dot, 0, 0);
  lv_obj_clear_flag(g_marker_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_marker_dot, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);

  g_marker_label = lv_label_create(g_chart);
  lv_obj_add_style(g_marker_label, &styles::price, 0);
  lv_label_set_text(g_marker_label, "");
  lv_obj_add_flag(g_marker_label, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(g_marker_label, LV_OBJ_FLAG_HIDDEN);
}

void rebuild_logo(const String& symbol) {
  lv_obj_clean(g_logo_slot);
  logos::make(g_logo_slot, symbol, 48);
}

// Pick the smallest step S from kSteps such that (range / S) <= 5, so we end
// up with 4–6 labels. Falls back to the largest step if the range is huge.
float pick_step(float range) {
  if (range <= 0) return 1.0f;
  for (float s : kSteps) {
    if (range / s <= 5.0f) return s;
  }
  return kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
}

void render_history(const History& h) {
  if (h.symbol != g_symbol || h.closes.empty()) return;
  lv_obj_add_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);

  float lo = h.closes.front(), hi = h.closes.front();
  for (float v : h.closes) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (hi - lo < 0.01f) { hi = lo + 0.5f; lo -= 0.5f; }

  // Snap min/max to "nice" multiples of the chosen step before labelling so
  // the user sees round numbers (e.g. 180/185/190/195) instead of the raw
  // data range (e.g. 180.0/183.6/187.3/190.9).
  float step    = pick_step(hi - lo);
  float lo_snap = floorf(lo / step) * step;
  float hi_snap = ceilf (hi / step) * step;
  if (hi_snap <= lo_snap) hi_snap = lo_snap + step;
  int n_y_ticks = (int)lroundf((hi_snap - lo_snap) / step) + 1;
  if (n_y_ticks < 2) n_y_ticks = 2;
  if (n_y_ticks > MAX_Y_TICKS) n_y_ticks = MAX_Y_TICKS;

  lv_coord_t chart_min = (lv_coord_t)(lo_snap * 100);
  lv_coord_t chart_max = (lv_coord_t)(hi_snap * 100);
  lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, chart_min, chart_max);

  // Render Y-tick labels first so we can measure the widest one, then size
  // the left gutter to match. Labels for unused slots in the pool stay hidden.
  char buf[40];
  int widest = 0;
  int xlabel_h = 14;   // muted Montserrat 12 line height, used as fallback
  for (int j = 0; j < MAX_Y_TICKS; ++j) {
    lv_obj_t* lbl = g_y_labels[j];
    if (j >= n_y_ticks) {
      lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    float v = hi_snap - j * step;
    if (step >= 1.0f) snprintf(buf, sizeof(buf), "%d",  (int)lroundf(v));
    else              snprintf(buf, sizeof(buf), "%.1f", v);
    lv_label_set_text(lbl, buf);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(lbl);
    int w = lv_obj_get_width(lbl);
    if (w > widest)  widest   = w;
    int hh = lv_obj_get_height(lbl);
    if (hh > xlabel_h) xlabel_h = hh;
  }

  // Reserve a left gutter for the Y labels and a bottom strip for the X
  // labels so neither pair overlaps the plot area or each other.
  int gutter   = widest   + 4;
  int pad_btm  = xlabel_h + 2;
  int content_h = CHART_H - pad_btm;
  lv_obj_set_style_pad_left  (g_chart, gutter,  LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(g_chart, pad_btm, LV_PART_MAIN);
  lv_obj_update_layout(g_chart);

  // Y labels: x flush in the gutter, y maps the snapped value linearly to the
  // (now shorter) content height so the bottom label no longer sits in the
  // X-axis strip.
  for (int j = 0; j < n_y_ticks; ++j) {
    int   lh    = lv_obj_get_height(g_y_labels[j]);
    int   y_px  = (content_h * j) / (n_y_ticks - 1);
    int   y_top = y_px - lh / 2;
    if (y_top < 0)            y_top = 0;
    if (y_top + lh > content_h) y_top = content_h - lh;
    lv_obj_set_pos(g_y_labels[j], 0, y_top);
  }

  // Catmull-Rom smooth: interpolate closes → static buffer, then feed chart.
  int n = (int)h.closes.size();
  static float g_cr_buf[CR_MAX_OUT];
  int out_n = (n > 1) ? (n - 1) * CR_FACTOR + 1 : n;
  if (out_n > CR_MAX_OUT) out_n = CR_MAX_OUT;
  util::catmull_rom_interpolate(h.closes.data(), n, g_cr_buf, out_n, CR_FACTOR);

  lv_chart_set_point_count(g_chart, out_n);
  for (int i = 0; i < out_n; ++i) {
    lv_chart_set_value_by_id(g_chart, g_ser, i,
                             (lv_coord_t)(g_cr_buf[i] * 100));
  }

  // Precompute pixel positions for the area-fill draw callback so it never
  // touches lv_chart_get_point_pos_by_id mid-draw (that path reads chart
  // internal state that's in flux during DRAW_MAIN_BEGIN, and on x86 the
  // sim trips a heap-corruption guard).
  int content_w = CHART_W - gutter;
  float span = hi_snap - lo_snap;
  if (span <= 0.0f) span = 1.0f;
  g_fill_n = out_n;
  g_fill_bottom_y = content_h;
  for (int i = 0; i < out_n; ++i) {
    g_fill_x[i] = gutter + (content_w * i) / (out_n - 1);
    float ynorm = 1.0f - (g_cr_buf[i] - lo_snap) / span;
    int   y     = (int)lroundf(ynorm * content_h);
    if (y < 0)         y = 0;
    if (y > content_h) y = content_h;
    g_fill_y[i] = y;
  }

  lv_chart_refresh(g_chart);

  float last  = h.closes.back();
  float first = h.closes.front();
  float change = first != 0.0f ? (last - first) / first * 100.0f : 0.0f;
  bool up = change >= 0;

  snprintf(buf, sizeof(buf), "%.2f", last);
  lv_label_set_text(g_price, buf);

  snprintf(buf, sizeof(buf), "%s %+.2f%%",
           up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, change);
  lv_label_set_text(g_change, buf);
  lv_obj_set_style_text_color(
      g_change, up ? styles::up_color() : styles::dn_color(), 0);

  lv_color_t c = up ? styles::up_color() : styles::dn_color();
  g_line_color = c;
  lv_obj_set_style_line_color(g_chart, c, LV_PART_ITEMS);

  // X-axis labels — three ticks at indices [0, n/3, 2n/3] using each point's
  // own epoch (h.timestamps[idx]). The rightmost (n-1) tick is intentionally
  // omitted: the current-price marker sits there and a tick label would
  // collide with the marker label. The chart's left content edge starts at
  // pad_left = `gutter`; tick pixel x = gutter + idx_interp * content_w/(out_n-1).
  int n_native = n;
  int x_idx[X_TICK_COUNT] = { 0, n_native / 3, (2 * n_native) / 3 };
  time_t now = time(nullptr);
  for (int k = 0; k < X_TICK_COUNT; ++k) {
    int idx_native = x_idx[k];
    if (idx_native >= n_native) idx_native = n_native - 1;
    time_t t = 0;
    if ((int)h.timestamps.size() == n_native && h.timestamps[idx_native] > 0) {
      t = h.timestamps[idx_native];
    } else {
      // No per-point epoch — assume daily spacing back from now (sparkline
      // fallback path). Never let strftime decide the format.
      t = now - (time_t)(n_native - 1 - idx_native) * 86400;
    }
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    snprintf(buf, sizeof(buf), "%02d %s", tmv.tm_mday, kMonths[tmv.tm_mon]);
    lv_label_set_text(g_x_labels[k], buf);

    int idx_interp = idx_native * CR_FACTOR;
    int x_px       = gutter + (content_w * idx_interp) / (out_n - 1);
    lv_obj_update_layout(g_x_labels[k]);
    int lw = lv_obj_get_width (g_x_labels[k]);
    int lh = lv_obj_get_height(g_x_labels[k]);
    int lx = x_px - lw / 2;
    if (lx < gutter)       lx = gutter;
    if (lx + lw > CHART_W) lx = CHART_W - lw;
    // X labels live in the dedicated bottom strip, just below the plot.
    int ly = content_h + (pad_btm - lh) / 2;
    if (ly < content_h) ly = content_h;
    lv_obj_set_pos(g_x_labels[k], lx, ly);
  }

  // Current-price marker. Run after layout settles so the lookup sees the
  // post-gutter content width.
  lv_obj_set_style_bg_color(g_marker_dot, c, 0);
  lv_obj_remove_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);

  snprintf(buf, sizeof(buf), "%.2f", last);
  lv_label_set_text(g_marker_label, buf);
  lv_obj_set_style_text_color(g_marker_label, c, 0);
  lv_obj_remove_flag(g_marker_label, LV_OBJ_FLAG_HIDDEN);

  lv_obj_update_layout(g_chart);
  lv_obj_update_layout(g_marker_label);
  lv_point_t tip;
  lv_chart_get_point_pos_by_id(g_chart, g_ser, out_n - 1, &tip);

  lv_coord_t dot_x = tip.x - MARKER_DOT_SIZE / 2;
  lv_coord_t dot_y = tip.y - MARKER_DOT_SIZE / 2;
  if (dot_x < gutter)                    dot_x = gutter;
  if (dot_x > CHART_W - MARKER_DOT_SIZE) dot_x = CHART_W - MARKER_DOT_SIZE;
  if (dot_y < 0)                            dot_y = 0;
  if (dot_y > content_h - MARKER_DOT_SIZE)  dot_y = content_h - MARKER_DOT_SIZE;
  lv_obj_set_pos(g_marker_dot, dot_x, dot_y);

  lv_coord_t lw = lv_obj_get_width(g_marker_label);
  lv_coord_t lh = lv_obj_get_height(g_marker_label);
  lv_coord_t lx = tip.x - MARKER_DOT_SIZE / 2 - lw - 4;
  if (lx < gutter) lx = tip.x + MARKER_DOT_SIZE / 2 + 4;
  lv_coord_t ly = tip.y - lh / 2;
  if (ly < 0)             ly = 0;
  if (ly + lh > content_h) ly = content_h - lh;
  lv_obj_set_pos(g_marker_label, lx, ly);
}

}  // namespace

namespace detail_screen {

void show(QuoteStore* store, const String& symbol) {
  g_store = store;
  g_symbol = symbol;
  build_once();
  rebuild_logo(symbol);
  lv_label_set_text(g_title, symbol.c_str());
  lv_label_set_text(g_price, "—");
  lv_label_set_text(g_change, "loading…");
  lv_obj_set_style_text_color(g_change, styles::muted_color(), 0);
  for (int j = 0; j < MAX_Y_TICKS; ++j) {
    lv_label_set_text(g_y_labels[j], "");
    lv_obj_add_flag(g_y_labels[j], LV_OBJ_FLAG_HIDDEN);
  }
  for (int k = 0; k < X_TICK_COUNT; ++k) lv_label_set_text(g_x_labels[k], "");
  lv_obj_add_flag(g_marker_dot,   LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_marker_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
  g_fill_n = 0;  // suppress area-fill draw until new data is ready
  lv_chart_set_all_value(g_chart, g_ser, LV_CHART_POINT_NONE);
  g_store->requestHistory(symbol);
  g_active = true;
  g_rendered = false;
  lv_screen_load(g_scr);
}

void tick() {
  if (!g_active || g_rendered || !g_store) return;
  History h = g_store->history();
  if (h.symbol == g_symbol && !h.closes.empty()) {
    render_history(h);
    g_rendered = true;
    return;
  }
  for (const auto& q : g_store->snapshot()) {
    if (q.symbol == g_symbol && q.sparkline.size() >= 2) {
      History fallback;
      fallback.symbol = q.symbol;
      fallback.closes = q.sparkline;
      // No timestamps — render_history falls back to one-day spacing.
      render_history(fallback);
      return;
    }
  }
}

}  // namespace detail_screen
