#include "lvgl_bridge.h"

#include <Arduino.h>
#include <lvgl.h>

#include "lgfx_cyd.hpp"
#include "../config.h"

namespace {

LGFX_CYD* g_gfx = nullptr;

// 1/15 screen × 2 buffers in DMA-capable internal RAM. Smaller buffers leave
// headroom for the lodepng decoder and the larger font set.
constexpr uint32_t LINES = 16;
constexpr uint32_t BUF_PX = cfg::SCREEN_W * LINES;
DMA_ATTR lv_color_t g_buf1[BUF_PX];
DMA_ATTR lv_color_t g_buf2[BUF_PX];

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  g_gfx->startWrite();
  g_gfx->setAddrWindow(area->x1, area->y1, w, h);
  g_gfx->writePixels(reinterpret_cast<uint16_t*>(px), w * h);
  g_gfx->endWrite();
  lv_display_flush_ready(disp);
}

void touch_cb(lv_indev_t*, lv_indev_data_t* data) {
  uint16_t x = 0, y = 0;
  if (g_gfx->getTouch(&x, &y)) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

uint32_t tick_cb(void) { return millis(); }

}  // namespace

namespace bridge {

void init(LGFX_CYD* gfx) {
  g_gfx = gfx;
  lv_tick_set_cb(tick_cb);

  lv_display_t* disp = lv_display_create(cfg::SCREEN_W, cfg::SCREEN_H);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, g_buf1, g_buf2, sizeof(g_buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, touch_cb);
}

}  // namespace bridge
