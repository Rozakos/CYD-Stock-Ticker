#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace logos {

// Allocates the reusable runtime PNG decoder before UI widgets fragment the
// heap. Runtime logos can still fall back to a badge if this fails.
bool prepareRuntimeDecoder();

// Frees the reusable PNG decoder after cached runtime logos have been decoded.
// The small ARGB logo cache remains available for future widget rebuilds.
void releaseRuntimeDecoder();

// Frees decoded runtime logos after their owning LVGL widgets have been
// deleted. Call immediately after cleaning a screen/list that used them.
void clearRuntimeCache();

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
