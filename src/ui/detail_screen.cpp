#include "detail_screen.h"

#include <math.h>
#include <time.h>

#include "../config.h"
#include "../util/area_fill.h"
#include "../util/interpolate.h"
#include "../net/quote_store.h"
#include "list_screen.h"
#include "logos.h"
#include "styles.h"

namespace {

// Compact single-line header (logo + symbol + price + change) lets us
// squeeze the 24 px range-button row in between header and chart on the
// 320×240 panel — anything taller and the chart drops below ~140 px.
static constexpr int HEADER_H    = 36;
static constexpr int LOGO_SIZE   = 32;
static constexpr int BTN_ROW_H   = 24;
static constexpr int CARD_Y      = HEADER_H + 2 + BTN_ROW_H + 2;             // 64
static constexpr int CARD_H      = cfg::SCREEN_H - 16 - CARD_Y;              // 160
static constexpr int CHART_W     = cfg::SCREEN_W - 32;                        // 288
static constexpr int CHART_H     = CARD_H - 16;                               // 144
static constexpr int MAX_Y_TICKS = 8;
static constexpr int X_TICK_COUNT = 3;
static constexpr int CR_FACTOR    = 5;
static constexpr int CR_MAX_OUT   = (cfg::HISTORY_POINTS - 1) * CR_FACTOR + 1;
static constexpr int MARKER_DOT_SIZE = 8;
// If the last data point lands in the top N% of the plot area, drop the
// marker label below the dot so it stays clear of the top Y-tick label.
static constexpr int MARKER_TOP_BAND_PCT = 15;
static constexpr int MARKER_OPA_TOP      = (LV_OPA_70);

// Range buttons. Button index → API range token. The label seen on the
// btnmatrix is `g_range_labels[]`; the value sent in `?range=` is
// `g_range_api[]`. Keep both arrays in lock-step.
static constexpr int kNumRanges       = 5;
static constexpr int kDefaultRangeIdx = 3;     // "1M"
static const char* g_range_labels[] = {
    "1D", "5D", "1W", "1M", "6M", ""
};
static const char* g_range_api[kNumRanges] = {
    "1d", "5d", "1w", "1mo", "6mo"
};

static const char* kMonths[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

static const float kSteps[] = {
    0.5f, 1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000, 2000, 5000
};

QuoteStore* g_store    = nullptr;
String      g_symbol;
bool        g_active     = false;
bool        g_rendered   = false;
bool        g_loading    = false;
bool        g_showed_err = false;
// g_range_idx = range currently DISPLAYED on the chart (what the chart's
// data and the active-button highlight both reflect).
// g_pending_range_idx = range the user most recently asked for; a fetch
// for this range is in flight. The active highlight only catches up to
// g_pending_range_idx once new data has rendered (or the user taps a
// different range, replacing the request).
int         g_range_idx          = kDefaultRangeIdx;
int         g_pending_range_idx  = kDefaultRangeIdx;

lv_obj_t* g_scr        = nullptr;
lv_obj_t* g_card       = nullptr;
lv_obj_t* g_header     = nullptr;
lv_obj_t* g_logo_slot  = nullptr;
lv_obj_t* g_title      = nullptr;
lv_obj_t* g_back_hint  = nullptr;
lv_obj_t* g_price      = nullptr;
lv_obj_t* g_change     = nullptr;
lv_obj_t* g_btn_row    = nullptr;
lv_obj_t* g_chart      = nullptr;
lv_chart_series_t* g_ser = nullptr;
lv_obj_t* g_spinner    = nullptr;
lv_obj_t* g_err_label  = nullptr;
lv_obj_t* g_y_labels[MAX_Y_TICKS]  = {};
lv_obj_t* g_x_labels[X_TICK_COUNT] = {};
lv_obj_t* g_range_btns[kNumRanges] = {};
lv_obj_t* g_marker_dot   = nullptr;
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

void start_history_fetch();   // forward decl — used by on_range_clicked
void apply_range_styles();    // forward decl — updates active button styling

void on_range_clicked(lv_event_t* e) {
  // user_data is the button index baked in at registration time. Using a
  // baked-in index avoids any hit-test ambiguity that a single multi-cell
  // widget (lv_buttonmatrix) introduced on the actual device, where the
  // ButtonMatrix internal coords ended up mirrored relative to the touch
  // driver.
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= kNumRanges) return;
  if (idx == g_range_idx) return;   // already on this range
  // Do NOT commit g_range_idx yet — keep the previous range "active"
  // visually so the highlighted button always matches the data on the
  // chart. The new range becomes active in render_history once the new
  // data has been drawn. start_history_fetch reads g_pending_range_idx
  // for the API call.
  g_pending_range_idx = idx;
  start_history_fetch();
}

// Apply CHECKED-state visuals: active button gets the accent bg colour,
// inactive ones get the muted card-frame colour. Called whenever
// g_range_idx changes and when the up/down accent flips in render_history.
void apply_range_styles() {
  lv_color_t accent = g_line_color;
  for (int i = 0; i < kNumRanges; ++i) {
    lv_obj_t* b = g_range_btns[i];
    if (!b) continue;
    if (i == g_range_idx) {
      lv_obj_set_style_bg_color(b, accent,          LV_PART_MAIN);
      lv_obj_set_style_bg_opa  (b, LV_OPA_80,       LV_PART_MAIN);
      lv_obj_set_style_text_color(b, lv_color_hex(0x0b0f17), LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(b, lv_color_hex(0x2a3548), LV_PART_MAIN);
      lv_obj_set_style_bg_opa  (b, LV_OPA_COVER,            LV_PART_MAIN);
      lv_obj_set_style_text_color(b, lv_color_hex(0xe7eef7), LV_PART_MAIN);
    }
  }
}

void start_history_fetch() {
  if (!g_store || !g_symbol.length()) return;
  g_loading    = true;
  g_showed_err = false;
  g_rendered   = false;
  if (g_spinner)   lv_obj_remove_flag(g_spinner,   LV_OBJ_FLAG_HIDDEN);
  if (g_err_label) lv_obj_add_flag   (g_err_label, LV_OBJ_FLAG_HIDDEN);
  // Hide the marker / fill data while the new range loads so we don't
  // briefly render stale labels at the old data's coordinates.
  if (g_marker_dot) lv_obj_add_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);
  g_fill_n = 0;
  g_store->requestHistory(g_symbol, g_range_api[g_pending_range_idx]);
}

// Area-fill is drawn by util::draw_polyline_fill (shared with the list
// sparklines). The helper rasterizes the polygon row-by-row so every
// pixel in a given row uses the same alpha — vertical bands cannot
// appear at trapezoid seams (which was the symptom of every previous
// per-x-column / per-trapezoid attempt).
//
// Drawn on LV_EVENT_DRAW_MAIN_BEGIN so the chart's line renders on top.
void chart_area_fill_cb(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_BEGIN) return;
  if (g_fill_n < 2) return;

  lv_obj_t* chart = (lv_obj_t*)lv_event_get_target(e);
  if (!chart) return;

  lv_layer_t* layer = lv_event_get_layer(e);
  if (!layer) return;

  lv_area_t obj_coords;
  lv_obj_get_coords(chart, &obj_coords);
  util::draw_polyline_fill(layer, g_fill_x, g_fill_y, g_fill_n,
                           obj_coords.x1, obj_coords.y1,
                           g_fill_bottom_y, g_line_color, MARKER_OPA_TOP);
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

  // Compact single-row header: [logo] [symbol] [price] (flex grow) [chg %] [back hint]
  g_header = lv_obj_create(g_scr);
  lv_obj_remove_style_all(g_header);
  lv_obj_set_size(g_header, cfg::SCREEN_W - 16, HEADER_H);
  lv_obj_align(g_header, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_flex_flow(g_header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_header, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(g_header, 6, 0);
  lv_obj_clear_flag(g_header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_header, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_logo_slot = lv_obj_create(g_header);
  lv_obj_remove_style_all(g_logo_slot);
  lv_obj_set_size(g_logo_slot, LOGO_SIZE, LOGO_SIZE);
  lv_obj_clear_flag(g_logo_slot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(g_logo_slot, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_title = lv_label_create(g_header);
  lv_obj_add_style(g_title, &styles::sym_small, 0);

  g_price = lv_label_create(g_header);
  lv_obj_add_style(g_price, &styles::price, 0);
  lv_label_set_text(g_price, "—");

  g_change = lv_label_create(g_header);
  lv_obj_add_style(g_change, &styles::price, 0);
  lv_label_set_text(g_change, "loading…");

  g_back_hint = lv_label_create(g_header);
  lv_obj_add_style(g_back_hint, &styles::muted, 0);
  lv_label_set_text(g_back_hint, LV_SYMBOL_LEFT " tap");

  // Range button row — 5 individual lv_button widgets inside a flex
  // container. Each button's index is baked into its CLICKED event's
  // user_data, so the index-to-API mapping cannot be confused by hit-
  // test mirroring inside a multi-cell widget. (lv_buttonmatrix was
  // previously used and reported mirrored clicks on the actual device
  // even though it rendered LTR — switching to discrete buttons removes
  // that whole class of bug.)
  g_btn_row = lv_obj_create(g_scr);
  lv_obj_remove_style_all(g_btn_row);
  lv_obj_set_size(g_btn_row, cfg::SCREEN_W - 16, BTN_ROW_H);
  lv_obj_align(g_btn_row, LV_ALIGN_TOP_LEFT, 0, HEADER_H + 2);
  lv_obj_set_flex_flow(g_btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(g_btn_row, 4, 0);
  lv_obj_clear_flag(g_btn_row, LV_OBJ_FLAG_SCROLLABLE);

  const int btn_w = ((cfg::SCREEN_W - 16) - 4 * (kNumRanges - 1)) / kNumRanges;
  for (int i = 0; i < kNumRanges; ++i) {
    lv_obj_t* b = lv_button_create(g_btn_row);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, btn_w, BTN_ROW_H);
    lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(b, &lv_font_montserrat_12, LV_PART_MAIN);

    lv_obj_t* lbl = lv_label_create(b);
    lv_label_set_text(lbl, g_range_labels[i]);
    lv_obj_center(lbl);

    // Firmware touch X is mirrored by the panel calibration. Keep the sim's
    // hit-test natural, but reverse the firmware user_data so a physical tap
    // on the left-to-right visual button requests that same logical range.
#if SIM_BUILD
    int logical_idx = i;
#else
    int logical_idx = kNumRanges - 1 - i;
#endif
    lv_obj_add_event_cb(b, on_range_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)logical_idx);
    g_range_btns[i] = b;
  }

  g_card = lv_obj_create(g_scr);
  lv_obj_remove_style_all(g_card);
  lv_obj_add_style(g_card, &styles::card, 0);
  lv_obj_set_size(g_card, cfg::SCREEN_W - 16, CARD_H);
  lv_obj_align(g_card, LV_ALIGN_TOP_LEFT, 0, CARD_Y);
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
  lv_obj_set_style_opa(g_spinner, LV_OPA_50, 0);

  // Centered "no data" label, shown on fetch failure. Stays inside the
  // card so the user can still tap a different range button — which sits
  // OUTSIDE the card, so it remains tappable while this label is up.
  g_err_label = lv_label_create(g_card);
  lv_obj_add_style(g_err_label, &styles::price, 0);
  lv_obj_set_style_text_color(g_err_label, styles::dn_color(), 0);
  lv_label_set_text(g_err_label, "no data");
  lv_obj_center(g_err_label);
  lv_obj_add_flag(g_err_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_err_label, LV_OBJ_FLAG_EVENT_BUBBLE);

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
}

void rebuild_logo(const String& symbol) {
  lv_obj_clean(g_logo_slot);
  // Pass the actual slot size so the embedded ARGB image (48 px source)
  // and the letter-badge fallback both render at the slot's pixel size.
  // logos::make already calls lv_image_set_scale for the embedded path
  // and sets badge size + font directly for the fallback path.
  logos::make(g_logo_slot, symbol, LOGO_SIZE);
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
  if (h.range.length() && h.range != g_range_api[g_pending_range_idx]) return;
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

  // Monotone cubic Hermite smooth (PCHIP) — kills the wiggles/overshoot
  // that uniform Catmull-Rom was producing on noisy daily close series.
  int n = (int)h.closes.size();
  static float g_cr_buf[CR_MAX_OUT];
  int out_n = (n > 1) ? (n - 1) * CR_FACTOR + 1 : n;
  if (out_n > CR_MAX_OUT) out_n = CR_MAX_OUT;
  util::monotone_cubic_interpolate(h.closes.data(), n, g_cr_buf, out_n, CR_FACTOR);

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
  float chart_change = first != 0.0f ? (last - first) / first * 100.0f : 0.0f;
  log_i("[ui] %s %s rendered: first=%.2f last=%.2f change=%+.2f%% pts=%lu",
        g_symbol.c_str(), g_range_api[g_pending_range_idx],
        first, last, chart_change, (unsigned long)h.closes.size());

  // The detail header reflects the selected chart window. The list screen
  // still shows the quote's published day change; here 5D/1W/1M/6M should
  // show gain/loss from this history's first point to its last point.
  float change = chart_change;
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
  // Use lv_chart_set_series_color rather than the style approach — the
  // series carries its own stored colour set at lv_chart_add_series time
  // and the LV_PART_ITEMS line_color style does NOT override it for the
  // series stroke. Going through the documented setter keeps the line
  // colour in lock-step with g_line_color (which the polygon fill helper
  // and the marker dot also consume).
  lv_chart_set_series_color(g_chart, g_ser, c);
  // Refresh the active range button's bg colour to match the up/down accent.
  apply_range_styles();

  // X-axis labels — three ticks at native indices [0, n/3, 2n/3] using
  // each point's own epoch. The format switches on the API-reported
  // interval: "intraday" → HH:MM (gmtime, no local TZ on device),
  // anything else → DD MMM (English month from kMonths).
  bool intraday = (h.interval == "intraday");
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
      // Fallback synthetic spacing — daily for the sparkline fallback,
      // 1 minute for the intraday case so multiple ticks still differ.
      time_t step_s = intraday ? (time_t)60 : (time_t)86400;
      t = now - (time_t)(n_native - 1 - idx_native) * step_s;
    }
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    if (intraday) {
      snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
      snprintf(buf, sizeof(buf), "%02d %s", tmv.tm_mday, kMonths[tmv.tm_mon]);
    }
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

  // Current-price marker (chart child). Dot only — the floating price
  // label was redundant with the header price readout and collided with
  // the top Y-tick label / line crossings. lv_chart_get_point_pos_by_id
  // returns chart-local coords so positions apply directly.
  lv_obj_set_style_bg_color(g_marker_dot, c, 0);
  lv_obj_remove_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);

  lv_obj_update_layout(g_chart);
  lv_point_t tip;
  lv_chart_get_point_pos_by_id(g_chart, g_ser, out_n - 1, &tip);

  lv_coord_t dot_x = tip.x - MARKER_DOT_SIZE / 2;
  lv_coord_t dot_y = tip.y - MARKER_DOT_SIZE / 2;
  if (dot_x < 0)                        dot_x = 0;
  if (dot_x > plot_w - MARKER_DOT_SIZE) dot_x = plot_w - MARKER_DOT_SIZE;
  if (dot_y < 0)                        dot_y = 0;
  if (dot_y > plot_h - MARKER_DOT_SIZE) dot_y = plot_h - MARKER_DOT_SIZE;
  lv_obj_set_pos(g_marker_dot, dot_x, dot_y);
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
  lv_obj_add_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);
  lv_chart_set_all_value(g_chart, g_ser, LV_CHART_POINT_NONE);
  // Reset range to default each time the user opens detail. Mark the
  // default button checked; clear all other CHECKED flags first.
  g_range_idx         = kDefaultRangeIdx;
  g_pending_range_idx = kDefaultRangeIdx;
  apply_range_styles();
  g_active = true;
  start_history_fetch();
  lv_screen_load(g_scr);
}

void tick() {
  if (!g_active || !g_store) return;

  // Error state — pop the "no data" label, stop showing the spinner,
  // leave buttons interactive for a retry. Revert the pending range to
  // the displayed one so a retap of the same button re-issues the fetch
  // (dedupe in on_range_clicked compares against g_range_idx).
  if (g_loading && g_store->historyError()) {
    g_loading = false;
    g_pending_range_idx = g_range_idx;
    if (g_spinner)   lv_obj_add_flag   (g_spinner,   LV_OBJ_FLAG_HIDDEN);
    if (g_err_label) lv_obj_remove_flag(g_err_label, LV_OBJ_FLAG_HIDDEN);
    g_showed_err = true;
    return;
  }

  if (g_rendered) return;
  History h = g_store->history();
  if (h.symbol == g_symbol && h.range == g_range_api[g_pending_range_idx] &&
      !h.closes.empty()) {
    if (g_spinner)   lv_obj_add_flag(g_spinner,   LV_OBJ_FLAG_HIDDEN);
    if (g_err_label) lv_obj_add_flag(g_err_label, LV_OBJ_FLAG_HIDDEN);
    render_history(h);
    // New data is on screen — commit the pending range as active. The
    // highlighted button now matches what the chart shows.
    g_range_idx = g_pending_range_idx;
    apply_range_styles();
    g_loading  = false;
    g_rendered = true;
    return;
  }
  // Fall back to the daily sparkline so the user sees SOMETHING while
  // the proper history fetch is in flight. Don't latch g_rendered so
  // the real result still takes over when it arrives.
  for (const auto& q : g_store->snapshot()) {
    if (q.symbol == g_symbol && q.sparkline.size() >= 2) {
      History fallback;
      fallback.symbol   = q.symbol;
      fallback.interval = "daily";
      fallback.closes   = q.sparkline;
      render_history(fallback);
      return;
    }
  }
}

bool active() { return g_active; }

}  // namespace detail_screen
