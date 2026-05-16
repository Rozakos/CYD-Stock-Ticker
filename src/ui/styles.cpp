#include "styles.h"

namespace styles {

lv_style_t row;
lv_style_t row_pressed;
lv_style_t sym;
lv_style_t sym_small;
lv_style_t price;
lv_style_t price_big;
lv_style_t pct_up;
lv_style_t pct_dn;
lv_style_t muted;
lv_style_t status_bar;
lv_style_t status_text;
lv_style_t card;
lv_style_t badge;
lv_style_t badge_text;

static lv_color_t c_up    = lv_color_hex(0xa78bfa);  // violet  — sparklines, chart, WiFi icon
static lv_color_t c_dn    = lv_color_hex(0x60a5fa);  // sky-blue — sparklines, chart (down)
static lv_color_t c_lbl_up = lv_color_hex(0x4ade80); // green — positive % labels
static lv_color_t c_lbl_dn = lv_color_hex(0xf87171); // red   — negative % labels
static lv_color_t c_bg    = lv_color_hex(0x0b0f17);
static lv_color_t c_card  = lv_color_hex(0x16202d);
static lv_color_t c_text  = lv_color_hex(0xe7eef7);
static lv_color_t c_muted = lv_color_hex(0x8a98ad);

lv_color_t up_color()     { return c_up; }
lv_color_t dn_color()     { return c_dn; }
lv_color_t lbl_up_color() { return c_lbl_up; }
lv_color_t lbl_dn_color() { return c_lbl_dn; }
lv_color_t bg_color()    { return c_bg; }
lv_color_t card_color()  { return c_card; }
lv_color_t muted_color() { return c_muted; }

struct BrandEntry { const char* sym; uint32_t rgb; };
static const BrandEntry kBrandTable[] = {
    {"AAPL", 0xa3aaad}, {"MSFT", 0x00a4ef}, {"NVDA", 0x76b900},
    {"TSLA", 0xcc0000}, {"GOOG", 0x4285f4}, {"GOOGL",0x4285f4},
    {"AMZN", 0xff9900}, {"META", 0x1877f2}, {"NFLX", 0xe50914},
    {"AMD",  0xed1c24}, {"INTC", 0x0071c5}, {"IBM",  0x1f70c1},
    {"DIS",  0x113ccf}, {"BA",   0x0033a0}, {"SHOP", 0x95bf47},
    {"UBER", 0x000000}, {"COIN", 0x0052ff}, {"PYPL", 0x003087},
    {"SQ",   0x3e4348}, {"SPOT", 0x1db954}, {"PLTR", 0x101a23},
    {"ORCL", 0xc74634}, {"CRM",  0x00a1e0}, {"CSCO", 0x1ba0d7},
    {"BABA", 0xff6a00}, {"SBUX", 0x006241}, {"NKE",  0x111111},
    {"WMT",  0x0071ce}, {"COST", 0xe31837}, {"HD",   0xf96302},
    {"BTC",  0xf7931a}, {"ETH",  0x627eea}, {"DOGE", 0xc2a633},
};

// Small palette for unknown symbols, picked by symbol-character hash.
static const uint32_t kFallback[] = {
    0x60a5fa, 0xa78bfa, 0xf472b6, 0xfb923c, 0x34d399,
    0xfbbf24, 0x22d3ee, 0xf87171, 0xa3e635, 0x818cf8,
};

lv_color_t brand_color(const String& symbol) {
  for (const auto& e : kBrandTable) {
    if (symbol.equalsIgnoreCase(e.sym)) return lv_color_hex(e.rgb);
  }
  uint32_t h = 2166136261u;  // FNV-1a
  for (size_t i = 0; i < symbol.length(); ++i) {
    h ^= (uint8_t)symbol[i];
    h *= 16777619u;
  }
  return lv_color_hex(kFallback[h % (sizeof(kFallback) / sizeof(kFallback[0]))]);
}

void init() {
  lv_style_init(&row);
  lv_style_set_bg_color(&row, c_card);
  lv_style_set_bg_opa(&row, LV_OPA_COVER);
  lv_style_set_border_width(&row, 0);
  lv_style_set_radius(&row, 8);
  lv_style_set_pad_hor(&row, 8);
  lv_style_set_pad_ver(&row, 4);
  lv_style_set_text_color(&row, c_text);

  lv_style_init(&row_pressed);
  lv_style_set_bg_color(&row_pressed, lv_color_hex(0x223349));

  lv_style_init(&sym);
  lv_style_set_text_font(&sym, &lv_font_montserrat_24);
  lv_style_set_text_color(&sym, c_text);

  lv_style_init(&sym_small);
  lv_style_set_text_font(&sym_small, &lv_font_montserrat_18);
  lv_style_set_text_color(&sym_small, c_text);

  lv_style_init(&price);
  lv_style_set_text_font(&price, &lv_font_montserrat_18);
  lv_style_set_text_color(&price, c_text);

  lv_style_init(&price_big);
  lv_style_set_text_font(&price_big, &lv_font_montserrat_28);
  lv_style_set_text_color(&price_big, c_text);

  lv_style_init(&pct_up);
  lv_style_set_text_font(&pct_up, &lv_font_montserrat_16);
  lv_style_set_text_color(&pct_up, c_lbl_up);

  lv_style_init(&pct_dn);
  lv_style_set_text_font(&pct_dn, &lv_font_montserrat_16);
  lv_style_set_text_color(&pct_dn, c_lbl_dn);

  lv_style_init(&muted);
  lv_style_set_text_font(&muted, &lv_font_montserrat_12);
  lv_style_set_text_color(&muted, c_muted);

  lv_style_init(&status_bar);
  lv_style_set_bg_color(&status_bar, lv_color_hex(0x07090d));
  lv_style_set_bg_opa(&status_bar, LV_OPA_COVER);
  lv_style_set_border_width(&status_bar, 0);
  lv_style_set_pad_hor(&status_bar, 8);
  lv_style_set_pad_ver(&status_bar, 2);

  lv_style_init(&status_text);
  lv_style_set_text_font(&status_text, &lv_font_montserrat_14);
  lv_style_set_text_color(&status_text, c_muted);

  lv_style_init(&card);
  lv_style_set_bg_color(&card, c_card);
  lv_style_set_bg_opa(&card, LV_OPA_COVER);
  lv_style_set_border_width(&card, 0);
  lv_style_set_radius(&card, 10);
  lv_style_set_pad_all(&card, 8);

  lv_style_init(&badge);
  lv_style_set_radius(&badge, LV_RADIUS_CIRCLE);
  lv_style_set_bg_opa(&badge, LV_OPA_COVER);
  lv_style_set_border_width(&badge, 0);
  lv_style_set_pad_all(&badge, 0);

  lv_style_init(&badge_text);
  lv_style_set_text_font(&badge_text, &lv_font_montserrat_14);
  lv_style_set_text_color(&badge_text, lv_color_hex(0x0b0f17));
}

}  // namespace styles
