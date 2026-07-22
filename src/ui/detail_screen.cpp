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
// Label pool size. Non-1D ranges use 3 evenly-spaced ticks; the extended-hours
// 1D view uses 4, snapped to the window edges and the two session dividers
// (04:00 / 09:30 / 16:00 / 20:00 ET) so the divider lines are self-labelling.
// Extra slots are hidden rather than deleted.
static constexpr int X_TICK_COUNT   = 4;
static constexpr int X_TICKS_PLAIN  = 3;
static constexpr int CR_FACTOR    = 5;
static constexpr int CR_MAX_OUT   = (cfg::HISTORY_POINTS - 1) * CR_FACTOR + 1;
static constexpr int MARKER_DOT_SIZE = 8;
// If the last data point lands in the top N% of the plot area, drop the
// marker label below the dot so it stays clear of the top Y-tick label.
static constexpr int MARKER_TOP_BAND_PCT = 15;
static constexpr int MARKER_OPA_TOP      = (LV_OPA_70);

// --- Heap budget for opening the detail view --------------------------------
// Measured on device: the widget tree built by build_once() (chart + spinner +
// 7 range buttons + 11 tick labels + header) costs ~20 KB, the persistent TLS
// session holds ~40 KB, and the whole heap once the list is up is ~69 KB. So
// 40 + 20 leaves ~9 KB — and a failed malloc from there aborts inside newlib's
// lazy lock init, which is the "tap a row, instant reboot, never reaches the
// chart" symptom.
//
// Do NOT try to buy room by dropping the TLS session. It can only be
// established at boot, when the largest free block is ~73 KB; mid-run the
// largest block is capped around 32 KB and mbedTLS fails the handshake with
// -32512 (SSL_ALLOC_FAILED) forever after. Dropping it costs quotes AND
// history permanently and walks the device into the dead-fetch watchdog
// reboot. The room comes from the list rows instead (see release_list_rows
// below) — they are invisible while detail is up and cost nothing to rebuild.
//
// Hard floor: if even that leaves too little, refuse the open. Staying on the
// list is a bad outcome; aborting into a reboot is a worse one.
static constexpr uint32_t OPEN_MIN_FREE = 14 * 1024;

// Range buttons. Button index -> API range token. The label seen on the
// button is `g_range_labels[]`; the value sent in `?range=` is
// `g_range_api[]`. Keep both arrays in lock-step.
static constexpr int kNumRanges       = 7;
static constexpr int kDefaultRangeIdx = 0;     // "1D"
static const char* g_range_labels[] = {
    "1D", "1W", "1M", "6M", "1Y", "5Y", "Max"
};
static const char* g_range_api[kNumRanges] = {
    "1d", "1w", "1mo", "6mo", "1y", "5y", "max"
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
// True from show() until the first real chart for this symbol has rendered.
// Gates the daily-sparkline fallback so it only fills the initial blank — on
// a later range switch we keep a clean blank+spinner instead of flickering
// the sparkline in before the requested range arrives.
bool        g_first_load = true;
// g_range_idx = range currently DISPLAYED on the chart (what the chart's
// data and the active-button highlight both reflect).
// g_pending_range_idx = range the user most recently asked for; a fetch
// for this range is in flight. The active highlight only catches up to
// g_pending_range_idx once new data has rendered (or the user taps a
// different range, replacing the request).
int         g_range_idx          = kDefaultRangeIdx;
int         g_pending_range_idx  = kDefaultRangeIdx;
// Generation of the requestHistory call whose result we're waiting on.
// render only fires when the stored History carries this gen — that's what
// lets a silent same-range auto-refresh tell fresh data from the stale
// window already in the store.
uint32_t    g_wait_gen           = 0;
// Auto-refresh: when a quote refresh lands while a window is on screen,
// the displayed range is silently re-requested and re-rendered in place
// (no blank/spinner); a failed silent fetch keeps the stale window and
// retries on the next quote refresh instead of surfacing the error label.
bool        g_silent             = false;
time_t      g_last_quote_seen    = 0;


lv_obj_t* g_scr        = nullptr;
lv_obj_t* g_card       = nullptr;
lv_obj_t* g_header     = nullptr;
lv_obj_t* g_logo_slot  = nullptr;
lv_obj_t* g_title      = nullptr;
lv_obj_t* g_back_btn   = nullptr;
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
// Regular open/close divider lines for the extended-hours 1D view, in
// chart-local X. -1 = not drawn (any non-1D range, or a server that gave us
// no window bounds). Drawn in the same callback as the area fill, so they
// cost no widgets and no heap.
int32_t g_divider_x[2]    = {-1, -1};

// Tear the whole detail tree down and hand its ~9 KB back to the allocator.
// Leaving it resident (which is what build_once's "build once, keep forever"
// used to do) dropped steady-state free heap from ~21 KB to ~12 KB for the
// rest of the boot — below netTask's starvation floor, so quotes stopped
// updating permanently and the next allocation spike aborted.
//
// Runs from lv_async_call, never straight out of the back button's event
// handler: lv_obj_delete walks the tree firing DELETE callbacks, and the
// button we are dispatching from is inside that tree.
void destroy_async(void*) {
  lv_obj_t* scr = g_scr;
  if (!scr) return;
  // Clear every cached pointer BEFORE the delete, so a tick() or a queued
  // event that lands mid-teardown cannot touch a half-freed widget.
  g_scr = nullptr;
  g_card = g_header = g_logo_slot = g_title = g_back_btn = nullptr;
  g_price = g_change = g_btn_row = g_chart = g_spinner = g_err_label = nullptr;
  g_marker_dot = nullptr;
  g_ser = nullptr;
  for (int j = 0; j < MAX_Y_TICKS; ++j)  g_y_labels[j]  = nullptr;
  for (int k = 0; k < X_TICK_COUNT; ++k) g_x_labels[k]  = nullptr;
  for (int i = 0; i < kNumRanges; ++i)   g_range_btns[i] = nullptr;
  g_fill_n = 0;
  g_divider_x[0] = g_divider_x[1] = -1;

  lv_obj_delete(scr);
  log_i("[ui] detail freed: heap free=%u largest=%u",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
}

void on_back(lv_event_t*) {
  g_active   = false;
  g_loading  = false;
  g_rendered = false;
  g_silent   = false;
  lv_screen_load(list_screen::screen());
  lv_async_call(destroy_async, nullptr);
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
  // Commit the pending range and highlight its button IMMEDIATELY so the tap
  // gets instant visual acknowledgement (the data and accent colour still
  // catch up in render_history). g_range_idx — the "displayed" range — only
  // moves once the new data is actually on screen.
  g_pending_range_idx = idx;
  apply_range_styles();
  start_history_fetch();
}

// Apply CHECKED-state visuals: the pending (just-tapped) button gets the
// accent bg colour, inactive ones get the muted card-frame colour. Tracking
// g_pending_range_idx (not g_range_idx) means the highlight follows the tap
// instantly, before the fetch completes — the user sees which range they
// asked for right away. Called on tap, on error-revert, and when the up/down
// accent flips in render_history.
void apply_range_styles() {
  lv_color_t accent = g_line_color;
  for (int i = 0; i < kNumRanges; ++i) {
    lv_obj_t* b = g_range_btns[i];
    if (!b) continue;
    if (i == g_pending_range_idx) {
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
  // Clear the previous range's curve and pop a prominent spinner so the
  // switch reads as "loading" right away instead of looking frozen until the
  // network round-trip lands. The marker/fill go with it (stale geometry).
  lv_chart_set_all_value(g_chart, g_ser, LV_CHART_POINT_NONE);
  g_fill_n = 0;
  g_divider_x[0] = g_divider_x[1] = -1;
  // Hide the time axis too: the labels describe the window we just blanked,
  // and leaving them up means a 1D->longer-range switch shows session times
  // under an empty chart (and strands 1D's 4th tick, which the 3-tick ranges
  // never overwrite).
  for (int k = 0; k < X_TICK_COUNT; ++k) {
    if (g_x_labels[k]) lv_obj_add_flag(g_x_labels[k], LV_OBJ_FLAG_HIDDEN);
  }
  if (g_marker_dot) lv_obj_add_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);
  if (g_err_label)  lv_obj_add_flag(g_err_label,  LV_OBJ_FLAG_HIDDEN);
  if (g_spinner) {
    lv_obj_remove_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_spinner);   // above the (now blank) chart
  }
  lv_obj_invalidate(g_chart);
  g_silent   = false;
  g_wait_gen = g_store->requestHistory(g_symbol, g_range_api[g_pending_range_idx]);
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

  lv_obj_t* chart = (lv_obj_t*)lv_event_get_target(e);
  if (!chart) return;

  lv_layer_t* layer = lv_event_get_layer(e);
  if (!layer) return;

  lv_area_t obj_coords;
  lv_obj_get_coords(chart, &obj_coords);

  // Session dividers first, so the area fill and the line both sit on top of
  // them — they are background furniture, not data.
  for (int i = 0; i < 2; ++i) {
    if (g_divider_x[i] < 0) continue;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(0x8fa3bf);
    d.opa   = LV_OPA_30;
    d.width = 1;
    d.p1.x  = obj_coords.x1 + g_divider_x[i];
    d.p1.y  = obj_coords.y1;
    d.p2.x  = obj_coords.x1 + g_divider_x[i];
    d.p2.y  = obj_coords.y1 + g_fill_bottom_y;
    lv_draw_line(layer, &d);
  }

  if (g_fill_n < 2) return;
  util::draw_polyline_fill(layer, g_fill_x, g_fill_y, g_fill_n,
                           obj_coords.x1, obj_coords.y1,
                           g_fill_bottom_y, g_line_color, MARKER_OPA_TOP);
}

void build_once() {
  if (g_scr) return;
  log_i("[ui] detail build: heap free=%u largest=%u",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  g_scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(g_scr, styles::bg_color(), 0);
  lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_scr, 8, 0);
  lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);
  // No whole-screen tap handler — tapping the chart/header must NOT navigate
  // back. Only the explicit back button (top-right of the header) returns to
  // the list.

  // Compact single-row header: [logo] [symbol] [price] (flex grow) [chg %] [back btn]
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

  // Spacer with flex-grow pushes the back button to the far right of the
  // header so the logo/symbol/price/change stay packed at the left.
  lv_obj_t* spacer = lv_obj_create(g_header);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_height(spacer, 1);
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_add_flag(spacer, LV_OBJ_FLAG_EVENT_BUBBLE);

  // Real tappable back button (top-right). The only way back to the list.
  g_back_btn = lv_button_create(g_header);
  lv_obj_remove_style_all(g_back_btn);
  lv_obj_set_size(g_back_btn, 34, HEADER_H - 4);
  lv_obj_set_style_radius(g_back_btn, 6, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_back_btn, lv_color_hex(0x2a3548), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_back_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_ext_click_area(g_back_btn, 10);   // generous hit-box on a small panel
  lv_obj_add_event_cb(g_back_btn, on_back, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* back_lbl = lv_label_create(g_back_btn);
  lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xe7eef7), LV_PART_MAIN);
  lv_obj_center(back_lbl);

  // Range button row — individual lv_button widgets inside a flex
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

    // Touch X is no longer mirrored (fixed in lgfx_cyd.hpp), so the button's
    // logical index matches its visual position directly — no per-platform
    // reversal needed.
    lv_obj_add_event_cb(b, on_range_clicked, LV_EVENT_CLICKED,
                        (void*)(intptr_t)i);
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
  lv_obj_set_size(g_spinner, 40, 40);
  lv_obj_center(g_spinner);
  lv_obj_set_style_opa(g_spinner, LV_OPA_COVER, 0);   // prominent, not a faint hint

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
  // Need at least 2 points: the interpolation, fill, marker and X-label code
  // all divide by (count - 1). A single-point history would divide by zero.
  if (h.symbol != g_symbol || h.closes.size() < 2) return;
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
  int out_n = (n - 1) * CR_FACTOR + 1;   // n >= 2 guaranteed above
  if (out_n > CR_MAX_OUT) out_n = CR_MAX_OUT;
  util::monotone_cubic_interpolate(h.closes.data(), n, g_cr_buf, out_n, CR_FACTOR);

  float span = hi_snap - lo_snap;
  if (span <= 0.0f) span = 1.0f;

  // Progressive 1D: the X axis represents a FIXED time window and the line
  // fills only the elapsed-so-far left portion (Revolut-style). We spread the
  // chart over `total_slots` evenly across that window, put the resampled data
  // in the first `active_n` (= elapsed fraction) slots, and leave the rest
  // LV_CHART_POINT_NONE so the line simply stops at "now".
  //
  // The window is the extended-hours span (04:00-20:00 ET) when the server
  // supplies it, with the regular open/close drawn as dividers inside it.
  // Falling back to the regular session alone keeps older servers working.
  //
  // Every other range fills the whole width: total_slots == active_n == out_n.
  bool   progressive = (h.range == "1d");
  static float slot_val[CR_MAX_OUT];
  int    total_slots, active_n;
  time_t win_open = 0, win_close = 0;      // X-axis bounds
  time_t sess_open = 0, sess_close = 0;    // divider positions (0 = none)
  if (progressive) {
    time_t first_ts = h.timestamps.empty() ? 0 : h.timestamps.front();
    time_t last_ts  = h.timestamps.empty() ? 0 : h.timestamps.back();
    win_open  = h.window_open;
    win_close = h.window_close;
    if (win_open > 0 && win_close > win_open) {
      // Extended window: the regular bounds become dividers, but only when
      // they actually fall inside it.
      if (h.session_open  > win_open && h.session_open  < win_close)
        sess_open = h.session_open;
      if (h.session_close > win_open && h.session_close < win_close)
        sess_close = h.session_close;
    } else {
      // No window from the server — axis spans the regular session as before,
      // and there is nothing to divide.
      win_open  = h.session_open;
      win_close = h.session_close;
    }
    // Last-resort fallback: assume a 6.5h regular session from the first point.
    if (win_open <= 0 || win_close <= win_open) {
      win_open  = first_ts > 0 ? first_ts : last_ts;
      win_close = win_open + (time_t)23400;   // 6.5h regular US session
      sess_open = sess_close = 0;
    }
    float frac = 1.0f;
    if (win_close > win_open && last_ts > 0) {
      frac = (float)(last_ts - win_open) / (float)(win_close - win_open);
    }
    if (frac < 0.02f) frac = 0.02f;   // always show at least a sliver
    if (frac > 1.0f)  frac = 1.0f;

    total_slots = CR_MAX_OUT;
    active_n    = (int)lroundf(frac * (total_slots - 1)) + 1;
    if (active_n < 2)           active_n = 2;
    if (active_n > total_slots) active_n = total_slots;

    // Resample the smoothed elapsed curve (g_cr_buf[0..out_n-1]) into the
    // first `active_n` slots by linear sampling — out_n is already dense, so
    // a linear pass keeps the curve smooth.
    for (int i = 0; i < active_n; ++i) {
      float pos = (active_n > 1)
                      ? (float)i / (float)(active_n - 1) * (float)(out_n - 1)
                      : 0.0f;
      int   i0  = (int)pos;
      int   i1  = (i0 + 1 < out_n) ? i0 + 1 : i0;
      float fr  = pos - (float)i0;
      slot_val[i] = g_cr_buf[i0] * (1.0f - fr) + g_cr_buf[i1] * fr;
    }
  } else {
    total_slots = out_n;
    active_n    = out_n;
    for (int i = 0; i < out_n; ++i) slot_val[i] = g_cr_buf[i];
  }

  lv_chart_set_point_count(g_chart, total_slots);
  for (int i = 0; i < total_slots; ++i) {
    if (i < active_n)
      lv_chart_set_value_by_id(g_chart, g_ser, i,
                               (lv_coord_t)(slot_val[i] * 100));
    else
      lv_chart_set_value_by_id(g_chart, g_ser, i, LV_CHART_POINT_NONE);
  }

  // Precompute pixel positions for the area-fill callback. Chart-local
  // coords — no gutter offset, since the chart obj itself is at (gutter,
  // 0) inside the card. Only the active (elapsed) slots become polygon
  // vertices, so the fill stops under the line's right end.
  g_fill_n = active_n;
  g_fill_bottom_y = plot_h;
  for (int i = 0; i < active_n; ++i) {
    g_fill_x[i] = (plot_w * i) / (total_slots - 1);
    float ynorm = 1.0f - (slot_val[i] - lo_snap) / span;
    int   y     = (int)lroundf(ynorm * plot_h);
    if (y < 0)      y = 0;
    if (y > plot_h) y = plot_h;
    g_fill_y[i] = y;
  }

  // Divider positions, in the same chart-local X space as the fill. Mapped
  // from wall-clock into the window rather than from slot indices, so they
  // stay put as the elapsed portion grows through the day.
  g_divider_x[0] = g_divider_x[1] = -1;
  if (progressive && win_close > win_open) {
    const time_t win_span = win_close - win_open;
    const time_t marks[2] = { sess_open, sess_close };
    for (int i = 0; i < 2; ++i) {
      if (marks[i] <= win_open || marks[i] >= win_close) continue;
      g_divider_x[i] =
          (int32_t)((long long)plot_w * (marks[i] - win_open) / win_span);
    }
  }

  lv_chart_refresh(g_chart);

  float last  = h.closes.back();
  float first = h.closes.front();
  float chart_change = first != 0.0f ? (last - first) / first * 100.0f : 0.0f;
  log_i("[ui] %s %s rendered: first=%.2f last=%.2f change=%+.2f%% pts=%lu",
        g_symbol.c_str(), g_range_api[g_pending_range_idx],
        first, last, chart_change, (unsigned long)h.closes.size());

  // The detail header reflects the selected chart window: every longer
  // range shows gain/loss from this history's first point to its last.
  // 1D is the exception: like the list rows it compares against the previous
  // session close, so a gap-down day that climbs off the open still reads red.
  Quote live;
  bool have_live = false;
  for (const auto& q : g_store->snapshot()) {
    if (q.symbol == g_symbol && q.fresh) {
      live = q;
      have_live = true;
      break;
    }
  }
  float change = chart_change;
  if (progressive) {
    // Prefer the API's prev_close. The old path back-derived it from the live
    // quote (last / (1 + changePct/100)), which compounded that percentage's
    // rounding — and silently produced nothing when no fresh quote was around.
    float prev_close = h.prev_close;
    if (isnan(prev_close) && have_live && !isnan(live.changePct) &&
        live.changePct > -100.0f) {
      prev_close = live.last / (1.0f + live.changePct / 100.0f);
    }
    if (!isnan(prev_close) && prev_close > 0.0f) {
      change = (last - prev_close) / prev_close * 100.0f;
    }
  }
  bool up = change >= 0;

  // The header price stays on the live quote (extended-hours print when one
  // is active) so a silent chart refresh doesn't stomp it back to the
  // session close; the window's last point is only the fallback before the
  // first quote lands.
  float header_price = last;
  if (have_live) {
    header_price = live.extended() ? live.extPrice : live.last;
  }

  snprintf(buf, sizeof(buf), "%.2f", header_price);
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

  // X-axis labels. The format reflects the REQUESTED range (the user's
  // mental model), not the API's `interval` field.
  //   1D  → FIXED wall-clock ticks so the axis always represents the whole
  //         window no matter how much has elapsed. With an extended-hours
  //         window that is four ticks SNAPPED to the window edges and the two
  //         session dividers (04:00 / 09:30 / 16:00 / 20:00 ET) — even spacing
  //         would put labels next to the divider lines instead of on them, and
  //         09:30 / 16:00 are the two times a reader actually looks for.
  //         Without a window it degrades to the old open / mid / close.
  //         Shown in device-local time (TZ is configured at boot).
  //   else → three data-driven DD MMM ticks sampled from the points. (For 5d
  //         the backend returns intraday-resolution data spanning several
  //         calendar days, so a day component must be visible.)
  int card_w = CHART_W;   // card content width (card width - 2*pad_all)
  for (int k = 0; k < X_TICK_COUNT; ++k) {
    lv_obj_add_flag(g_x_labels[k], LV_OBJ_FLAG_HIDDEN);
  }
  if (progressive) {
    // Tick times, and the x position each one is pinned to. Dividers are
    // pinned to the divider pixel so label and line cannot drift apart.
    time_t t_ticks[X_TICK_COUNT];
    int    x_ticks[X_TICK_COUNT];
    int    n_ticks = 0;
    time_t win_span = win_close - win_open;
    if (g_divider_x[0] >= 0 || g_divider_x[1] >= 0) {
      t_ticks[n_ticks] = win_open;  x_ticks[n_ticks++] = 0;
      for (int i = 0; i < 2; ++i) {
        if (g_divider_x[i] < 0) continue;
        t_ticks[n_ticks] = (i == 0) ? sess_open : sess_close;
        x_ticks[n_ticks++] = g_divider_x[i];
      }
      t_ticks[n_ticks] = win_close; x_ticks[n_ticks++] = plot_w;
    } else {
      for (int k = 0; k < X_TICKS_PLAIN; ++k) {
        t_ticks[k] = win_open +
                     (time_t)((long long)win_span * k / (X_TICKS_PLAIN - 1));
        x_ticks[k] = (plot_w * k) / (X_TICKS_PLAIN - 1);
      }
      n_ticks = X_TICKS_PLAIN;
    }

    for (int k = 0; k < n_ticks; ++k) {
      time_t t = t_ticks[k];
      struct tm tmv;
#if defined(_WIN32)
      localtime_s(&tmv, &t);
#else
      localtime_r(&t, &tmv);
#endif
      snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
      lv_label_set_text(g_x_labels[k], buf);
      lv_obj_remove_flag(g_x_labels[k], LV_OBJ_FLAG_HIDDEN);

      int x_px = gutter + x_ticks[k];
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
  } else {
    int n_native = n;
    // First / middle / LAST point. Sampling {0, n/3, 2n/3} left the most
    // recent date (the right edge) unlabelled — so 1W never showed today and
    // 5Y/Max never showed the current year. Anchor the last tick to n-1.
    int x_idx[X_TICKS_PLAIN] = { 0, (n_native - 1) / 2, n_native - 1 };
    time_t now = time(nullptr);
    // Pick the tick format from how much wall-time the window covers. "DD MMM"
    // is meaningless once the window spans years (5Y / Max show points decades
    // apart) — switch to the year alone there; medium windows show month+year;
    // short windows keep day+month.
    long span_days = 0;
    if (h.timestamps.size() >= 2 && h.timestamps.front() > 0 &&
        h.timestamps.back() > h.timestamps.front()) {
      span_days = (long)((h.timestamps.back() - h.timestamps.front()) / 86400);
    }
    enum { FMT_DAY_MON, FMT_MON_YEAR, FMT_YEAR } xfmt =
        span_days > 730 ? FMT_YEAR : (span_days > 365 ? FMT_MON_YEAR : FMT_DAY_MON);
    for (int k = 0; k < X_TICKS_PLAIN; ++k) {
      int idx_native = x_idx[k];
      if (idx_native >= n_native) idx_native = n_native - 1;
      time_t t = 0;
      if ((int)h.timestamps.size() == n_native && h.timestamps[idx_native] > 0) {
        t = h.timestamps[idx_native];
      } else {
        // Fallback synthetic daily spacing (sparkline-fallback path has no ts).
        t = now - (time_t)(n_native - 1 - idx_native) * (time_t)86400;
      }
      struct tm tmv;
#if defined(_WIN32)
      gmtime_s(&tmv, &t);
#else
      gmtime_r(&t, &tmv);
#endif
      if (xfmt == FMT_YEAR) {
        snprintf(buf, sizeof(buf), "%d", tmv.tm_year + 1900);
      } else if (xfmt == FMT_MON_YEAR) {
        snprintf(buf, sizeof(buf), "%s %02d", kMonths[tmv.tm_mon],
                 (tmv.tm_year + 1900) % 100);
      } else {
        snprintf(buf, sizeof(buf), "%02d %s", tmv.tm_mday, kMonths[tmv.tm_mon]);
      }
      lv_label_set_text(g_x_labels[k], buf);
      lv_obj_remove_flag(g_x_labels[k], LV_OBJ_FLAG_HIDDEN);

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
  }

  // Current-price marker (chart child). Dot only — the floating price
  // label was redundant with the header price readout and collided with
  // the top Y-tick label / line crossings. lv_chart_get_point_pos_by_id
  // returns chart-local coords so positions apply directly.
  lv_obj_set_style_bg_color(g_marker_dot, c, 0);
  lv_obj_remove_flag(g_marker_dot, LV_OBJ_FLAG_HIDDEN);

  lv_obj_update_layout(g_chart);
  lv_point_t tip;
  // Marker sits at the last ELAPSED point (= the line's right end). For 1D
  // that's well left of the chart edge early in the session; for other ranges
  // active_n == total_slots so it's the rightmost point as before.
  lv_chart_get_point_pos_by_id(g_chart, g_ser, active_n - 1, &tip);

  lv_coord_t dot_x = tip.x - MARKER_DOT_SIZE / 2;
  lv_coord_t dot_y = tip.y - MARKER_DOT_SIZE / 2;
  if (dot_x < 0)                        dot_x = 0;
  if (dot_x > plot_w - MARKER_DOT_SIZE) dot_x = plot_w - MARKER_DOT_SIZE;
  if (dot_y < 0)                        dot_y = 0;
  if (dot_y > plot_h - MARKER_DOT_SIZE) dot_y = plot_h - MARKER_DOT_SIZE;
  lv_obj_set_pos(g_marker_dot, dot_x, dot_y);
}

void open_now(const String& symbol) {
  g_symbol = symbol;
  build_once();
  log_i("[ui] detail built: heap free=%u largest=%u",
        (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
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
  g_first_load        = true;   // allow the sparkline fallback to fill the blank
  // Baseline for the silent auto-refresh: don't treat the refresh that was
  // already on screen when the user opened detail as "new data landed".
  g_last_quote_seen   = g_store->lastUpdate();
  apply_range_styles();
  g_active = true;
  start_history_fetch();
  lv_screen_load(g_scr);
}

}  // namespace

namespace detail_screen {

void show(QuoteStore* store, const String& symbol) {
  g_store = store;
  // The list's rows are about to be covered by this screen, so their widgets
  // are dead weight for as long as detail is up. Free them BEFORE building —
  // that is where the headroom for the ~20 KB detail tree comes from now that
  // dropping the TLS session is off the table. list_screen::tick() rebuilds
  // them from the store the moment detail closes.
  list_screen::releaseRows();
  if (ESP.getFreeHeap() < OPEN_MIN_FREE) {
    log_e("[ui] detail open refused for %s: heap free=%u largest=%u below "
          "floor — staying on the list rather than risking an OOM abort",
          symbol.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
    return;
  }
  open_now(symbol);
}

void tick() {
  if (!g_active || !g_store) return;

  // Error state — pop the "no data" label, stop showing the spinner,
  // leave buttons interactive for a retry. Revert the pending range to
  // the displayed one so a retap of the same button re-issues the fetch
  // (dedupe in on_range_clicked compares against g_range_idx).
  if (g_loading && g_store->historyError()) {
    g_loading = false;
    if (g_silent) {
      // A failed silent refresh keeps the stale window on screen; the next
      // quote refresh retries. Only a user-initiated fetch surfaces the
      // error label.
      g_silent   = false;
      g_rendered = true;
      return;
    }
    g_pending_range_idx = g_range_idx;
    apply_range_styles();   // move the highlight back to the still-displayed range
    if (g_spinner)   lv_obj_add_flag   (g_spinner,   LV_OBJ_FLAG_HIDDEN);
    if (g_err_label) lv_obj_remove_flag(g_err_label, LV_OBJ_FLAG_HIDDEN);
    g_showed_err = true;
    return;
  }

  if (g_rendered) {
    // Window on screen and nothing in flight: when a quote refresh has
    // landed since the last request, silently re-request the displayed
    // range so the chart keeps tracking the live session (no blank, no
    // spinner — render_history redraws in place when the result arrives).
    time_t lu = g_store->lastUpdate();
    if (lu != g_last_quote_seen) {
      g_last_quote_seen = lu;
      g_silent   = true;
      g_loading  = true;
      g_rendered = false;
      g_wait_gen = g_store->requestHistory(g_symbol, g_range_api[g_range_idx]);
    }
    return;
  }
  History h = g_store->history();
  // Gen match = this is the answer to OUR latest request. Symbol+range
  // alone can't distinguish a silent same-range refresh from the stale
  // window already in the store.
  if (h.gen == g_wait_gen && h.symbol == g_symbol && !h.closes.empty()) {
    if (g_spinner)   lv_obj_add_flag(g_spinner,   LV_OBJ_FLAG_HIDDEN);
    if (g_err_label) lv_obj_add_flag(g_err_label, LV_OBJ_FLAG_HIDDEN);
    render_history(h);
    // New data is on screen — commit the pending range as active. The
    // highlighted button now matches what the chart shows.
    g_range_idx  = g_pending_range_idx;
    apply_range_styles();
    g_loading    = false;
    g_rendered   = true;
    g_silent     = false;
    g_first_load = false;   // future switches keep blank+spinner, no flicker
    return;
  }
  // First open only: fall back to the daily sparkline so the user sees
  // SOMETHING while the proper history fetch is in flight. On a range switch
  // we skip this — a clean blank + spinner is clearer than flickering the
  // sparkline in for a moment. Don't latch g_rendered so the real result
  // still takes over when it arrives.
  if (!g_first_load) return;
  // The fallback is a DAILY sparkline. For the 1D intraday view it would flash
  // a day-scale curve that doesn't match the real intraday shape before the
  // API call lands — show the spinner alone for 1D instead.
  if (String(g_range_api[g_pending_range_idx]) == "1d") return;
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
