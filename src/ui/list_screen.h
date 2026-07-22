#pragma once

#include <lvgl.h>

class QuoteStore;
class SettingsStore;

namespace list_screen {

// `settings` is borrowed for the lifetime of the screen; used by the
// WiFi-icon tap handler to clear creds + reboot into setup mode.
void build(QuoteStore* store, SettingsStore* settings = nullptr);
lv_obj_t* screen();
void tick();  // call from UI timer to refresh from store

// Delete the row widgets, keeping the screen itself. Called when the detail
// view opens: the rows are invisible underneath it, and their memory is what
// makes room for the detail tree on this heap. tick() rebuilds them from the
// store on the next pass once detail is no longer active.
void releaseRows();

}  // namespace list_screen
