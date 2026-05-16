#pragma once

#include <cstdint>
#include <string>

namespace sim_bridge {

enum class Mode { Headless, Window };

// Init LVGL display, input, and (if mode==Window) an SDL window. Also
// registers an LVGL FS driver for drive 'L' rooted at `data_root`.
void init(Mode mode, int width, int height, const std::string& data_root);

// Pump SDL events + lv_timer_handler. Returns false when the user closes the
// window. In headless mode this just runs LVGL timers and always returns true.
bool tick();

// Write the current framebuffer to a PNG file. Returns true on success.
bool dump_png(const std::string& path);

// Inject a synthetic touch click at the given DISPLAY coordinates (in
// device pixels, 0..width / 0..height). Returns after `down_ms` of press
// and a release event. Useful for scripted UI checks from sim_main.
void inject_click(int x, int y, int down_ms = 60);

void shutdown();

}  // namespace sim_bridge
