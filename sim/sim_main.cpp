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
#include "../src/net/web_admin_page.h"
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
  std::string      screen   = "list";  // list | detail | settings | wifi
  std::string      symbol   = "AAPL";
  int              warmup_ticks = 10;
  std::string      data_root    = "../../data";  // host path to logos (from sim/build/)
  std::string      click;          // "X,Y" — inject a tap after warmup
  std::string      longpress;       // "X,Y" — inject a long press (>~500 ms) after warmup
  std::string      web_settings_out;  // dump /settings HTML and exit
  bool             intraday = false;   // override history with intraday data
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
    else if (eat("--click",  a.click)) {}
    else if (eat("--longpress", a.longpress)) {}
    else if (eat("--web-settings", a.web_settings_out)) {}
    else if (s == "--intraday") a.intraday = true;
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

struct HtmlFileWriter {
  std::FILE* fp = nullptr;

  void print(const char* s) {
    if (fp && s) std::fputs(s, fp);
  }

  void print(const String& s) {
    print(s.c_str());
  }
};

void tick_active_screen() {
  if      (detail_screen::active())     detail_screen::tick();
  else if (settings_screen::active())   settings_screen::tick();
  else if (wifi_setup_screen::active()) wifi_setup_screen::tick();
  else                                  list_screen::tick();
}

void service_sim_history(QuoteStore& store) {
  HistoryRequest req;
  if (store.takePendingHistory(req)) {
    fake_data::seed_range_history(store, req.symbol.c_str(), req.range.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args = parse(argc, argv);

  if (!args.web_settings_out.empty()) {
    LittleFS.read_overlay = args.data_root;
    LittleFS.begin();

    SettingsStore settings;
    settings.begin("");

    std::FILE* fp = std::fopen(args.web_settings_out.c_str(), "wb");
    if (!fp) {
      std::fprintf(stderr, "HTML write failed: %s\n", args.web_settings_out.c_str());
      return 1;
    }

    HtmlFileWriter writer{fp};
    web_admin_page::write_settings_page(writer, settings);
    std::fclose(fp);
    std::fprintf(stderr, "wrote %s\n", args.web_settings_out.c_str());
    return 0;
  }

  sim_bridge::init(args.mode, cfg::SCREEN_W, cfg::SCREEN_H, args.data_root);

  // Let LittleFS find the project's logo PNGs as a read-only overlay.
  LittleFS.read_overlay = args.data_root;

  QuoteStore    store;
  SettingsStore settings;
  store.begin();
  settings.begin("");  // empty seed key; sim doesn't fetch.

  fake_data::seed(store);
  if (args.intraday) fake_data::seed_intraday(store, args.symbol.c_str());

  styles::init();
  settings_screen::init(&settings);
  list_screen::build(&store, &settings);

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
    service_sim_history(store);
    tick_active_screen();
    if (!sim_bridge::tick()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  auto inject_at = [&](const std::string& xy, int hold_ms, const char* label) {
    int cx = 0, cy = 0;
    auto comma = xy.find(',');
    if (comma == std::string::npos) {
      std::fprintf(stderr, "[sim] %s expects X,Y (got %s)\n", label, xy.c_str());
      return;
    }
    cx = std::atoi(xy.substr(0, comma).c_str());
    cy = std::atoi(xy.substr(comma + 1).c_str());
    std::fprintf(stderr, "[sim] inject %s at %d,%d (hold=%d ms)\n",
                 label, cx, cy, hold_ms);
    sim_bridge::inject_click(cx, cy, hold_ms);
    for (int i = 0; i < 5; ++i) {
      service_sim_history(store);
      tick_active_screen();
      sim_bridge::tick();
    }
  };

  if (!args.click.empty())     inject_at(args.click,     60,  "--click");
  // LVGL's long-press threshold defaults to 400 ms. 600 is comfortably above
  // that with one tick of headroom for the timer to fire.
  if (!args.longpress.empty()) inject_at(args.longpress, 600, "--longpress");

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
      // Only tick the active screen — list_screen::tick segfaults inside
      // its row-rebuild path when the list isn't currently loaded
      // (sim/README.md "Known issues"). The warmup loop above already
      // gates this; the main loop has to as well.
      service_sim_history(store);
      tick_active_screen();
      next += std::chrono::milliseconds(16);
      std::this_thread::sleep_until(next);
    }
  }

  sim_bridge::shutdown();
  return 0;
}
