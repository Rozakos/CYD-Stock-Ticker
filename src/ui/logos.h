#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace logos {

// Creates a square logo for `symbol` sized `size` x `size`.
// Resolves /logos/<SYMBOL>.png on LittleFS; if missing, draws a circular
// brand-colored badge with the first 1-2 letters of the symbol.
lv_obj_t* make(lv_obj_t* parent, const String& symbol, lv_coord_t size);

// Cheap "what would `make` render?" probe. Returns a stable signature
// that combines source kind (embedded / runtime PNG / badge) with file
// size for the runtime path. list_screen uses this to skip rebuilding
// the logo widget when nothing actually changed.
uint32_t signature(const String& symbol);

}  // namespace logos
