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
static constexpr int MAX_Y_TICKS = 8;
static constexpr int X_TICK_COUNT = 3;
static constexpr int CR_FACTOR    = 5;
static constexpr int CR_MAX_OUT   = (cfg::HISTORY_POINTS - 1) * CR_FACTOR + 1;
static constexpr int MARKER_DOT_SIZE = 8;
// If the last data point lands in the top N% of the plot area, drop the
// marker label below the dot so it stays clear of the top Y-tick label.
static constexpr int MARKER_TOP_BAND_PCT = 15;
static constexpr int MARKER_OPA_TOP      = (LV_OPA_70);

static const char* kMonths[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

static const float kSteps[] = {
    0.5f, 1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000, 2000, 5000
};

QuoteStore* g_store    = nullptr;
String      g_symbol;
bool        g_active   = false;
bool        g_rendered = false;

lv_obj_t* g_scr        = nullptr;
lv_obj_t* g_card       = nullptr;
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
lv_color_t g_line_color  = lv_color_hex(0x4ade80);

// Geometry handed to the area-fill draw callback. All chart-local
// coordinates — the callback adds the chart's absolute (cx, cy) offset.
int     g_fill_n          = 0;
int32_t g_fill_x[CR_MAX_OUT];
int32_t g_fill_y[CR_MAX_OUT];
int32_t g_fill_bottom_y   = 0;

void on_tap(lv_event_t*) {
  g_active = false;
  lv_screen_load(list_screen::screen());
}

// LVGL 9 line charts don't paint an area under the line natively. We fill
// it manually as a chain of vertical-gradient trapezoids (split into pairs
// of triangles) using EVERY interpolated point — stride=1 — so the top
// edge of the polygon is exactly the Catmull-Rom line, with no chords
// cutting across the curve.
//
// Each triangle's gradient stops are derived from its own bbox so the
// per-triangle local gradient still samples the SAME global profile
// (full opacity at chart top, transparent at chart bottom). Adjacent
// trapezoids therefore meet with matching opacity along the seam and
// there are no visible bands or vertical stripes.
//
// Drawn on LV_EVENT_DRAW_MAIN_BEGIN so the chart's line renders cleanly
// on top of the fill.
void chart_area_fill_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_BEGIN) return;
  if (g_fill_n < 2) return;

  lv_obj_t* chart = (lv_obj_t*)lv_event_get_target(e);
  if (!chart) return;

  lv_layer_t* layer = lv_event_get_layer(e);
  if (!layer) return;

  lv_area_t obj_coords;
  lv_obj_get_coords(chart, &obj_coords);
  const int32_t cx     = obj_coords.x1;
  const int32_t cy     = obj_coords.y1;
  const int32_t bot    = g_fill_bottom_y;     // chart-local bottom y
  const int32_t ay_bot = cy + bot;
  if (bot <= 0) return;

  // Linear ramp: line_color at full opacity (top of plot) → transparent
  // (bottom of plot). Each triangle's stops are sampled from this so all
  // triangles agree at any shared y.
  auto opa_at = [&](int32_t y_local) -> lv_opa_t {
    if (y_local <= 0)   return MARKER_OPA_TOP;
    if (y_local >= bot) return LV_OPA_TRANSP;
    int v = (MARKER_OPA_TOP * (bot - y_local)) / bot;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (lv_opa_t)v;
  };

  lv_draw_triangle_dsc_t tdsc;
  lv_draw_triangle_dsc_init(&tdsc);
  tdsc.color = g_line_color;
  tdsc.opa   = LV_OPA_COVER;
  tdsc.grad.dir         = LV_GRAD_DIR_VER;
  tdsc.grad.stops_count = 2;
  tdsc.grad.stops[0].color = g_line_color;
  tdsc.grad.stops[0].frac  = 0;
  tdsc.grad.stops[1].color = g_line_color;
  tdsc.grad.stops[1].frac  = 255;
  tdsc.grad.stops[1].opa   = opa_at(bot);   // same for every triangle's bottom stop

  for (int i = 0; i + 1 < g_fill_n; ++i) {
    int32_t y0 = g_fill_y[i];
    int32_t y1 = g_fill_y[i + 1];
    int32_t ax0 = cx + g_fill_x[i],     ay0 = cy + y0;
    int32_t ax1 = cx + g_fill_x[i + 1], ay1 = cy + y1;

    // Upper triangle: line[i], line[i+1], bottom_right. Its bbox top is
    // the higher of the two line points.
    int32_t t1_ytop = (y0 < y1) ? y0 : y1;
    tdsc.grad.stops[0].opa = opa_at(t1_ytop);
    tdsc.p[0].x = ax0; tdsc.p[0].y = ay0;
    tdsc.p[1].x = ax1; tdsc.p[1].y = ay1;
    tdsc.p[2].x = ax1; tdsc.p[2].y = ay_bot;
    lv_draw_triangle(layer, &tdsc);

    // Lower triangle: line[i], bottom_right, bottom_left. Its bbox top
    // is at line[i] (single vertex above bottom row).
    tdsc.grad.stops[0].opa = opa_at(y0);
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

  g_card = lv_obj_create(g_scr);
  lv_obj_remove_style_all(g_card);
  lv_obj_add_style(g_card, &styles::card, 0);
  lv_obj_set_size(g_card, cfg::SCREEN_W - 16, CARD_H);
  lv_obj_align(g_card, LV_ALIGN_TOP_LEFT, 0, 56);
  lv_obj_clear_flag(g_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_card, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Chart fills the card initially; render_history shrinks it so the
  // Y-label gutter (sibling labels on the card) can sit to the left and
  // the X-label strip can sit below.
  g_chart = lv_chart_create(g_card);
  lv_obj_set_size(g_chart, CHART_W, CHART_H);
  lv_obj_align(g_chart, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(g_chart, cfg::HISTORY_POINTS);
  lv_chart_set_div_line_count(g_chart, 5, 0);
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

  g_spinner = lv_spinner_create(g_card);
  lv_obj_set_size(g_spinner, 36, 36);
  lv_obj_center(g_spinner);

  // Y labels are children of the CARD (siblings of the chart). The
  // chart's pad_left wasn't reliable for carving out gutter space —
  // chart-child positions are relative to the content area, which
  // pad_left also shifts, so the labels ended up inside the plot. As
  // siblings positioned outside the chart's bounding box they're
  // safe.
  for (int j = 0; j < MAX_Y_TICKS; ++j) {
    lv_obj_t* lbl = lv_label_create(g_card);
    lv_obj_add_style(lbl, &styles::muted, 0);
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    g_y_labels[j] = lbl;
  }

  // X labels are also siblings of the chart — clearer separation
  // between plot area and the bottom strip.
  for (int k = 0; k < X_TICK_COUNT; ++k) {
    lv_obj_t* lbl = lv_label_create(g_card);
    lv_obj_add_style(lbl, &styles::muted, 0);
    lv_label_set_text(lbl, "");
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    g_x_labels[k] = lbl;
  }

  // Marker stays as a chart child so chart-local coords from
  // lv_chart_get_point_pos_by_id position it directly.
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

// Pick the smallest step S from kSteps such that range / S ≤ 5, so we end
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

  float step    = pick_step(hi - lo);
  float lo_snap = floorf(lo / step) * step;
  float hi_snap = ceilf (hi / step) * step;
  if (hi_snap <= lo_snap) hi_snap = lo_snap + step;
  int n_y_ticks = (int)lroundf((hi_snap - lo_snap) / step) + 1;
  if (n_y_ticks < 2) n_y_ticks = 2;
  if (n_y_ticks > MAX_Y_TICKS) n_y_ticks = MAX_Y_TICKS;

  lv_coord_t chart_min = (lv_coord_t)(lo_snap * 100);
  lv_coord_t chart_max = (lv_coord_t)(hi_snap * 100);

  // Pass 1: format Y labels and measure them so we know how wide to
  // make the gutter. Pool-extra slots stay hidden.
  char buf[40];
  int widest = 0;
  int label_h = 14;   // Montserrat 12 line height, used until we measure
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
    if (w > widest)  widest  = w;
    int hh = lv_obj_get_height(lbl);
    if (hh > label_h) label_h = hh;
  }

  int gutter  = widest  + 6;       // user spec: widest Y label + 6 px
  int pad_btm = label_h + 4;       // X label strip height
  int plot_w  = CHART_W - gutter;
  int plot_h  = CHART_H - pad_btm;
  if (plot_w < 1) plot_w = 1;
  if (plot_h < 1) plot_h = 1;

  // Resize and reposition the chart so the gutter / bottom strip live
  // OUTSIDE its bounding box.
  lv_obj_set_pos(g_chart, gutter, 0);
  lv_obj_set_size(g_chart, plot_w, plot_h);
  lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, chart_min, chart_max);

  // Pass 2: place Y labels in the gutter. Right-edge aligned to
  // gutter-4 so the digits line up regardless of label width.
  for (int j = 0; j < n_y_ticks; ++j) {
    int lh = lv_obj_get_height(g_y_labels[j]);
    int lw = lv_obj_get_width (g_y_labels[j]);
    int y_value_px = (plot_h * j) / (n_y_ticks - 1);
    int ly = y_value_px - lh / 2;
    if (ly < 0)             ly = 0;
    if (ly + lh > plot_h)   ly = plot_h - lh;
    int lx = (gutter - 4) - lw;
    if (lx < 0) lx = 0;
    lv_obj_set_pos(g_y_labels[j], lx, ly);
  }

  // Catmull-Rom smooth the close-prices, then feed the chart.
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

  // Precompute pixel positions for the area-fill callback. Chart-local
  // coords — no gutter offset, since the chart obj itself is at (gutter,
  // 0) inside the card. Every interpolated point becomes a polygon
  // vertex (stride=1).
  float span = hi_snap - lo_snap;
  if (span <= 0.0f) span = 1.0f;
  g_fill_n = out_n;
  g_fill_bottom_y = plot_h;
  for (int i = 0; i < out_n; ++i) {
    g_fill_x[i] = (plot_w * i) / (out_n - 1);
    float ynorm = 1.0f - (g_cr_buf[i] - lo_snap) / span;
    int   y     = (int)lroundf(ynorm * plot_h);
    if (y < 0)      y = 0;
    if (y > plot_h) y = plot_h;
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

  // X-axis labels — three ticks at native indices [0, n/3, 2n/3] using
  // each point's own epoch. Positioned in card-local coords below the
  // chart. card-local x = gutter + interp_idx * plot_w / (out_n - 1).
  int n_native = n;
  int x_idx[X_TICK_COUNT] = { 0, n_native / 3, (2 * n_native) / 3 };
  time_t now = time(nullptr);
  int card_w = CHART_W;   // card content width (card width - 2*pad_all)
  for (int k = 0; k < X_TICK_COUNT; ++k) {
    int idx_native = x_idx[k];
    if (idx_native >= n_native) idx_native = n_native - 1;
    time_t t = 0;
    if ((int)h.timestamps.size() == n_native && h.timestamps[idx_native] > 0) {
      t = h.timestamps[idx_native];
    } else {
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
    int x_px       = gutter + (plot_w * idx_interp) / (out_n - 1);
    lv_obj_update_layout(g_x_labels[k]);
    int lw = lv_obj_get_width (g_x_labels[k]);
    int lh = lv_obj_get_height(g_x_labels[k]);
    int lx = x_px - lw / 2;
    if (lx < 0)            lx = 0;
    if (lx + lw > card_w)  lx = card_w - lw;
    int ly = plot_h + (pad_btm - lh) / 2;
    if (ly < plot_h)       ly = plot_h;
    lv_obj_set_pos(g_x_labels[k], lx, ly);
  }

  // Current-price marker (chart child). lv_chart_get_point_pos_by_id
  // returns chart-local coords so positions apply directly.
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
  if (dot_x < 0)                        dot_x = 0;
  if (dot_x > plot_w - MARKER_DOT_SIZE) dot_x = plot_w - MARKER_DOT_SIZE;
  if (dot_y < 0)                        dot_y = 0;
  if (dot_y > plot_h - MARKER_DOT_SIZE) dot_y = plot_h - MARKER_DOT_SIZE;
  lv_obj_set_pos(g_marker_dot, dot_x, dot_y);

  lv_coord_t lw = lv_obj_get_width(g_marker_label);
  lv_coord_t lh = lv_obj_get_height(g_marker_label);
  lv_coord_t lx, ly;
  if (tip.y < (plot_h * MARKER_TOP_BAND_PCT) / 100) {
    // Tip is high in the chart — placing the label next to the dot
    // would collide with the topmost Y tick. Drop it under the dot.
    lx = tip.x - lw / 2;
    ly = tip.y + MARKER_DOT_SIZE / 2 + 2;
  } else {
    // Default: to the left of the dot, vertically centred. Flip to
    // the right only if the left placement would leave the chart.
    lx = tip.x - MARKER_DOT_SIZE / 2 - lw - 4;
    if (lx < 0) lx = tip.x + MARKER_DOT_SIZE / 2 + 4;
    ly = tip.y - lh / 2;
  }
  // User spec: right edge ≤ chart_right - 2 px.
  if (lx + lw > plot_w - 2) lx = plot_w - 2 - lw;
  if (lx < 0)             lx = 0;
  if (ly < 0)             ly = 0;
  if (ly + lh > plot_h)   ly = plot_h - lh;
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
  g_fill_n = 0;
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
      render_history(fallback);
      return;
    }
  }
}

}  // namespace detail_screen
