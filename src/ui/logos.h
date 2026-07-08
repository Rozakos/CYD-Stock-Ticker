#pragma once

#include <Arduino.h>
#include <lvgl.h>

#include <vector>

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

// Drops cached runtime logos for symbols NOT in `keep` (case-insensitive)
// and keeps the rest mounted-ready. Use this instead of clearRuntimeCache()
// when rebuilding rows for a (possibly) changed symbol set: decoded logos
// survive the rebuild, which matters because a mid-run re-decode needs a
// ~45 KB contiguous block that rarely exists once TLS is up (the boot-time
// prewarm below is usually the only decode that ever succeeds).
void pruneRuntimeCache(const std::vector<String>& keep);

// Decodes every cached /logos/<SYMBOL>.png for `symbols` into the runtime
// cache. Call at boot, between prepareRuntimeDecoder() and the first network
// activity: the decode needs ~45 KB of contiguous heap transiently, which is
// only reliably available before the TLS session and the row widgets carve
// up the boot-time block. Later make() calls then hit the cache for free.
void prewarmRuntimeCache(const std::vector<String>& symbols);

// Creates a square logo for `symbol` sized `size` x `size`.
// Resolves /logos/<SYMBOL>.png on LittleFS; if missing, draws a circular
// brand-colored badge with the first 1-2 letters of the symbol.
//
// `mountedSig` (optional) receives the signature of what was ACTUALLY mounted,
// which can differ from signature() when a runtime PNG decode is deferred or
// fails and a badge is shown instead. Callers that cache a per-widget
// signature must store this value (not signature()) so they retry the rebuild
// once the real logo becomes mountable.
lv_obj_t* make(lv_obj_t* parent, const String& symbol, lv_coord_t size,
               uint32_t* mountedSig = nullptr);

// Cheap "what would `make` render?" probe. Returns a stable signature
// that combines source kind (embedded / runtime PNG / badge) with file
// size for the runtime path. list_screen uses this to skip rebuilding
// the logo widget when nothing actually changed.
uint32_t signature(const String& symbol);

// Consumes (returns-and-clears) the "a runtime decode was deferred for lack
// of contiguous heap" signal, set by make() when it has to mount a badge even
// though the PNG is cached. The net task polls this and drops its persistent
// TLS session (~40 KB resident) so the contiguous block recovers before
// list_screen's periodic logo retry — otherwise the badge would stay up
// until a reboot. Safe to call from a different task than make()'s.
bool consumeDecodeStarved();

}  // namespace logos
