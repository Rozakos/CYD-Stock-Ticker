#include "logos.h"

#include <LittleFS.h>

#include "../display/fs_littlefs.h"
#include "logos_data.h"
#include "styles.h"

namespace {

String logoPath(const String& symbol) {
  String s = symbol;
  s.toUpperCase();
  return String("/logos/") + s + ".png";
}

lv_obj_t* makeBadge(lv_obj_t* parent, const String& symbol, lv_coord_t size) {
  lv_obj_t* badge = lv_obj_create(parent);
  lv_obj_set_size(badge, size, size);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(badge, styles::brand_color(symbol), 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_pad_all(badge, 0, 0);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

  String letters = symbol;
  letters.toUpperCase();
  if (letters.length() > 2) letters = letters.substring(0, 2);

  lv_obj_t* lbl = lv_label_create(badge);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0x0b0f17), 0);
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
  String up = symbol;
  up.toUpperCase();

  // Preferred path: compile-time C array (ARGB8888) — bypasses LittleFS
  // and the LVGL FS / lodepng pipeline entirely, so it renders correctly
  // both on the device and in the desktop sim.
  if (auto* dsc = logos_data::find(up.c_str())) {
    log_i("[logo] %s embedded", up.c_str());
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, dsc);
    // Scale the image to the requested slot size. 256 = 1.0x.
    if (dsc->header.w > 0) {
      int32_t scale = (size * 256) / dsc->header.w;
      if (scale != 256) lv_image_set_scale(img, scale);
    }
    lv_obj_set_size(img, size, size);
    return img;
  }

  // Legacy fallback: try LittleFS PNG path (kept so user-added logos via
  // `pio run -t uploadfs` still work without a rebuild).
  String path = logoPath(symbol);
  bool exists = false;
  size_t bytes = 0;
  {
    fs_littlefs::Guard g;
    exists = LittleFS.exists(path);
    if (exists) {
      File f = LittleFS.open(path, "r");
      if (f) {
        bytes = f.size();
        f.close();
      }
    }
  }
  if (exists) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_border_opa(box, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(box, 2, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* img = lv_image_create(box);
    String lvPath = String("L:") + path;
    lv_image_set_src(img, lvPath.c_str());
    int32_t iw = lv_image_get_src_width(img);
    int32_t ih = lv_image_get_src_height(img);
    if (iw > 0 && ih > 0) {
      log_i("[logo] %s LittleFS %s bytes=%u dims=%ldx%ld",
            up.c_str(), path.c_str(), (unsigned)bytes, (long)iw, (long)ih);
      int32_t inner = size - 4;
      int32_t scale = (inner * 256) / iw;
      if (scale != 256) lv_image_set_scale(img, scale);
      lv_obj_set_size(img, inner, inner);
      lv_obj_center(img);
      log_i("[logo] %s runtime image mounted scale=%ld inner=%ld",
            up.c_str(), (long)scale, (long)inner);
      return box;
    }
    log_w("[logo] %s decode failed %s bytes=%u dims=%ldx%ld",
          up.c_str(), path.c_str(), (unsigned)bytes, (long)iw, (long)ih);
    lv_obj_delete(box);
  }

  log_i("[logo] %s badge fallback", up.c_str());
  return makeBadge(parent, symbol, size);
}

}  // namespace logos
