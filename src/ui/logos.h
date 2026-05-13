#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace logos {

// Creates a square logo for `symbol` sized `size` x `size`.
// Resolves /logos/<SYMBOL>.png on LittleFS; if missing, draws a circular
// brand-colored badge with the first 1-2 letters of the symbol.
lv_obj_t* make(lv_obj_t* parent, const String& symbol, lv_coord_t size);

}  // namespace logos
