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

void shutdown();

}  // namespace sim_bridge
