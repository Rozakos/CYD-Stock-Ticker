#include "detail_screen.h"

#include "../config.h"
#include "../net/quote_store.h"
#include "list_screen.h"
#include "logos.h"
#include "styles.h"

namespace {

QuoteStore* g_store    = nullptr;
String      g_symbol;
bool        g_active   = false;
bool        g_rendered = false;

lv_obj_t* g_scr        = nullptr;
lv_obj_t* g_header     = nullptr;  // logo + symbol + tap-to-go-back hint
lv_obj_t* g_logo_slot  = nullptr;  // wrapper so we can rebuild logo per symbol
lv_obj_t* g_title      = nullptr;
lv_obj_t* g_back_hint  = nullptr;
lv_obj_t* g_price      = nullptr;
lv_obj_t* g_change     = nullptr;
lv_obj_t* g_chart      = nullptr;
lv_chart_series_t* g_ser = nullptr;
lv_obj_t* g_spinner    = nullptr;
lv_obj_t* g_hi_label   = nullptr;
lv_obj_t* g_lo_label   = nullptr;
lv_obj_t* g_range_label = nullptr;

void on_tap(lv_event_t*) {
  g_active = false;
  lv_screen_load(list_screen::screen());
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

  // Header strip: [logo] [symbol + change pct]            [< back]
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

  // Chart card
  lv_obj_t* card = lv_obj_create(g_scr);
  lv_obj_remove_style_all(card);
  lv_obj_add_style(card, &styles::card, 0);
  lv_obj_set_size(card, cfg::SCREEN_W - 16, cfg::SCREEN_H - 54 - 24 - 4);
  lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 56);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_chart = lv_chart_create(card);
  lv_obj_set_size(g_chart, cfg::SCREEN_W - 16 - 16, 120);
  lv_obj_align(g_chart, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(g_chart, cfg::HISTORY_POINTS);
  lv_chart_set_div_line_count(g_chart, 3, 0);
  lv_chart_set_update_mode(g_chart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_obj_set_style_size(g_chart, 0, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(g_chart, 2, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(g_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(g_chart, 0, 0);
  lv_obj_set_style_line_color(g_chart, lv_color_hex(0x2a3548), LV_PART_MAIN);
  lv_obj_set_style_line_opa(g_chart, LV_OPA_30, LV_PART_MAIN);
  // Area fill under the line.
  lv_obj_set_style_bg_opa(g_chart, LV_OPA_20, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_dir(g_chart, LV_GRAD_DIR_VER, LV_PART_ITEMS);
  lv_obj_set_style_bg_main_opa(g_chart, LV_OPA_30, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_opa(g_chart, LV_OPA_0, LV_PART_ITEMS);
  lv_obj_add_flag(g_chart, LV_OBJ_FLAG_EVENT_BUBBLE);

  g_ser = lv_chart_add_series(g_chart, styles::up_color(),
                              LV_CHART_AXIS_PRIMARY_Y);

  g_spinner = lv_spinner_create(card);
  lv_obj_set_size(g_spinner, 36, 36);
  lv_obj_center(g_spinner);

  // High / Low / Range row under chart.
  lv_obj_t* stats = lv_obj_create(card);
  lv_obj_remove_style_all(stats);
  lv_obj_set_size(stats, cfg::SCREEN_W - 16 - 16, 28);
  lv_obj_align(stats, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(stats, LV_OBJ_FLAG_EVENT_BUBBLE);

  auto add_stat = [&](const char* caption) -> lv_obj_t* {
    lv_obj_t* col = lv_obj_create(stats);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t* cap = lv_label_create(col);
    lv_obj_add_style(cap, &styles::muted, 0);
    lv_label_set_text(cap, caption);
    lv_obj_t* val = lv_label_create(col);
    lv_obj_add_style(val, &styles::price, 0);
    lv_label_set_text(val, "—");
    return val;
  };

  g_lo_label    = add_stat("LOW");
  g_range_label = add_stat("RANGE");
  g_hi_label    = add_stat("HIGH");
}

void rebuild_logo(const String& symbol) {
  lv_obj_clean(g_logo_slot);
  logos::make(g_logo_slot, symbol, 48);
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

  // Add 5% headroom so the line doesn't touch the edges.
  float pad = (hi - lo) * 0.05f;
  lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y,
                     (lv_coord_t)((lo - pad) * 100),
                     (lv_coord_t)((hi + pad) * 100));

  lv_chart_set_point_count(g_chart, h.closes.size());
  for (size_t i = 0; i < h.closes.size(); ++i) {
    lv_chart_set_value_by_id(g_chart, g_ser, i,
                             (lv_coord_t)(h.closes[i] * 100));
  }
  lv_chart_refresh(g_chart);

  float last = h.closes.back();
  float first = h.closes.front();
  float change = first != 0.0f ? (last - first) / first * 100.0f : 0.0f;
  bool up = change >= 0;

  char buf[40];
  snprintf(buf, sizeof(buf), "%.2f", last);
  lv_label_set_text(g_price, buf);

  snprintf(buf, sizeof(buf), "%s %+.2f%%",
           up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, change);
  lv_label_set_text(g_change, buf);
  lv_obj_set_style_text_color(
      g_change, up ? styles::up_color() : styles::dn_color(), 0);

  lv_color_t c = up ? styles::up_color() : styles::dn_color();
  lv_obj_set_style_line_color(g_chart, c, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(g_chart, c, LV_PART_ITEMS);

  snprintf(buf, sizeof(buf), "%.2f", hi);
  lv_label_set_text(g_hi_label, buf);
  snprintf(buf, sizeof(buf), "%.2f", lo);
  lv_label_set_text(g_lo_label, buf);
  snprintf(buf, sizeof(buf), "%.2f", hi - lo);
  lv_label_set_text(g_range_label, buf);
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
  lv_label_set_text(g_hi_label, "—");
  lv_label_set_text(g_lo_label, "—");
  lv_label_set_text(g_range_label, "—");
  lv_obj_remove_flag(g_spinner, LV_OBJ_FLAG_HIDDEN);
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
  }
}

}  // namespace detail_screen
