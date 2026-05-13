#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace styles {

extern lv_style_t row;
extern lv_style_t row_pressed;
extern lv_style_t sym;
extern lv_style_t sym_small;
extern lv_style_t price;
extern lv_style_t price_big;
extern lv_style_t pct_up;
extern lv_style_t pct_dn;
extern lv_style_t muted;
extern lv_style_t status_bar;
extern lv_style_t status_text;
extern lv_style_t card;
extern lv_style_t badge;
extern lv_style_t badge_text;

void init();

lv_color_t up_color();
lv_color_t dn_color();
lv_color_t bg_color();
lv_color_t card_color();
lv_color_t muted_color();

// Stable per-symbol accent color, used for the badge fallback and chart line.
lv_color_t brand_color(const String& symbol);

}  // namespace styles
