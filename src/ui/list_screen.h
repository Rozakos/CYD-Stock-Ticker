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

}  // namespace list_screen
