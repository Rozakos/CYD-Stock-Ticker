#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <lvgl.h>

#include "Arduino.h"
#include "LittleFS.h"
#include "sim_lvgl_bridge.h"
#include "fake_data.h"

#include "../src/config.h"
#include "../src/net/quote_store.h"
#include "../src/settings/settings_store.h"
#include "../src/ui/detail_screen.h"
#include "../src/ui/list_screen.h"
#include "../src/ui/settings_screen.h"
#include "../src/ui/styles.h"
#include "../src/ui/wifi_setup_screen.h"

namespace {

struct Args {
  sim_bridge::Mode mode    = sim_bridge::Mode::Window;
  std::string      png_out;        // empty = no PNG dump
  std::string      screen   = "list";  // list | detail | settings
  std::string      symbol   = "AAPL";
  int              warmup_ticks = 10;
  std::string      data_root    = "../../data";  // host path to logos (from sim/build/)
};

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    auto eat = [&](const char* k, std::string& out) {
      std::string prefix = std::string(k) + "=";
      if (s.rfind(prefix, 0) == 0) { out = s.substr(prefix.size()); return true; }
      return false;
    };
    if      (s == "--headless") a.mode = sim_bridge::Mode::Headless;
    else if (s == "--window")   a.mode = sim_bridge::Mode::Window;
    else if (eat("--out",    a.png_out)) {}
    else if (eat("--screen", a.screen)) {}
    else if (eat("--symbol", a.symbol)) {}
    else if (eat("--data",   a.data_root)) {}
    else {
      std::string ticks_val;
      if (eat("--ticks", ticks_val)) {
        a.warmup_ticks = std::atoi(ticks_val.c_str());
      }
    }
  }
  // If user asked for PNG but didn't pick a mode explicitly, prefer headless.
  if (!a.png_out.empty() && a.mode == sim_bridge::Mode::Window) {
    // leave as Window so user gets both — they can pass --headless to skip the window.
  }
  return a;
}

}  // namespace

int main(int argc, char** argv) {
  Args args = parse(argc, argv);

  sim_bridge::init(args.mode, cfg::SCREEN_W, cfg::SCREEN_H, args.data_root);

  // Let LittleFS find the project's logo PNGs as a read-only overlay.
  LittleFS.read_overlay = args.data_root;

  QuoteStore    store;
  SettingsStore settings;
  store.begin();
  settings.begin("");  // empty seed key; sim doesn't fetch.

  fake_data::seed(store);

  styles::init();
  settings_screen::init(&settings);
  list_screen::build(&store);

  // Pick which screen to land on.
  if (args.screen == "list") {
    lv_screen_load(list_screen::screen());
  } else if (args.screen == "detail") {
    detail_screen::show(&store, String(args.symbol.c_str()));
  } else if (args.screen == "settings") {
    settings_screen::show();
  } else if (args.screen == "wifi") {
    wifi_setup_screen::show(String("CYD-Setup-AB12"), String());
  } else {
    std::fprintf(stderr, "Unknown --screen=%s (list|detail|settings)\n", args.screen.c_str());
    return 2;
  }

  // Warm-up: pump LVGL + the per-screen ticks so the UI fully renders before
  // we either snapshot or hand control to the event loop.
  for (int i = 0; i < args.warmup_ticks; ++i) {
    // Always tick list so its rows are populated for when the user navigates to it.
    if (args.screen == "list") list_screen::tick();
    if (args.screen == "detail")   detail_screen::tick();
    if (args.screen == "settings") settings_screen::tick();
    if (args.screen == "wifi")     wifi_setup_screen::tick();
    if (!sim_bridge::tick()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (!args.png_out.empty()) {
    if (sim_bridge::dump_png(args.png_out)) {
      std::fprintf(stderr, "wrote %s\n", args.png_out.c_str());
    } else {
      std::fprintf(stderr, "PNG write failed: %s\n", args.png_out.c_str());
    }
  }

  if (args.mode == sim_bridge::Mode::Window) {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (sim_bridge::tick()) {
      list_screen::tick();
      detail_screen::tick();
      settings_screen::tick();
      next += std::chrono::milliseconds(16);
      std::this_thread::sleep_until(next);
    }
  }

  sim_bridge::shutdown();
  return 0;
}
