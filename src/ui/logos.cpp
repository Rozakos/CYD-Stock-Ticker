#include "logos.h"

#include <LittleFS.h>

#include "styles.h"

namespace {

String logoPath(const String& symbol) {
  String s = symbol;
  s.toUpperCase();
  return String("/logos/") + s + ".png";
}

lv_obj_t* makeBadge(lv_obj_t* parent, const String& symbol, lv_coord_t size) {
  lv_obj_t* badge = lv_obj_create(parent);
  lv_obj_remove_style_all(badge);
  lv_obj_add_style(badge, &styles::badge, 0);
  lv_obj_set_size(badge, size, size);
  lv_obj_set_style_bg_color(badge, styles::brand_color(symbol), 0);

  // 1 letter for 1-3 char symbols, 2 letters otherwise. Keeps the badge
  // readable at small sizes without overflowing.
  String letters = symbol;
  letters.toUpperCase();
  if (letters.length() > 2) letters = letters.substring(0, 2);

  lv_obj_t* lbl = lv_label_create(badge);
  lv_obj_add_style(lbl, &styles::badge_text, 0);
  if (size >= 48) {
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
  } else if (size >= 32) {
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
  } else {
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
  }
  lv_label_set_text(lbl, letters.c_str());
  lv_obj_center(lbl);
  return badge;
}

}  // namespace

namespace logos {

lv_obj_t* make(lv_obj_t* parent, const String& symbol, lv_coord_t size) {
  String path = logoPath(symbol);
  if (LittleFS.exists(path)) {
    lv_obj_t* img = lv_image_create(parent);
    String lvPath = String("L:") + path;
    lv_image_set_src(img, lvPath.c_str());
    lv_obj_set_size(img, size, size);
    // Scale the source to fit the slot. LVGL 9 uses 256 = 1.0x scale.
    // We don't know the source size, so request a square scaler target via
    // explicit image size; LVGL will use the inner_align to position it.
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CONTAIN);
    return img;
  }
  return makeBadge(parent, symbol, size);
}

}  // namespace logos
