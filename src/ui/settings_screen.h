#pragma once

class SettingsStore;

namespace settings_screen {

void init(SettingsStore* settings);  // call once at startup
void show();                          // load + populate
void tick();                          // refresh dynamic fields (RSSI, IP)

}  // namespace settings_screen
