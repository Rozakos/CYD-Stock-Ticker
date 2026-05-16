#include "sim_lvgl_bridge.h"

#include <SDL2/SDL.h>
#include <lvgl.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace sim_bridge {

namespace {

Mode               g_mode      = Mode::Headless;
int                g_w         = 0;
int                g_h         = 0;
std::string        g_data_root;

// Authoritative framebuffer (RGB565 — same layout LVGL hands us).
std::vector<uint16_t> g_fb;

// SDL surfaces only created in Window mode.
SDL_Window*   g_window   = nullptr;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture*  g_texture  = nullptr;
bool          g_quit     = false;

// LVGL draw buffers (partial mode).
constexpr uint32_t LINES = 32;
lv_color_t g_buf1[320 * LINES];
lv_color_t g_buf2[320 * LINES];

const auto kBoot = std::chrono::steady_clock::now();

uint32_t tick_cb() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - kBoot).count();
}

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px) {
  const int32_t w = area->x2 - area->x1 + 1;
  const int32_t h = area->y2 - area->y1 + 1;
  const uint16_t* src = reinterpret_cast<uint16_t*>(px);
  for (int32_t row = 0; row < h; ++row) {
    uint16_t* dst = &g_fb[(area->y1 + row) * g_w + area->x1];
    std::memcpy(dst, src + row * w, w * sizeof(uint16_t));
  }
  lv_display_flush_ready(disp);
}

bool g_mouse_down = false;
int  g_mouse_x = 0, g_mouse_y = 0;

void mouse_cb(lv_indev_t*, lv_indev_data_t* data) {
  data->state   = g_mouse_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = g_mouse_x;
  data->point.y = g_mouse_y;
}

// ---- LVGL FS driver: drive 'L' maps to <g_data_root>/<path> ---------------

void* fs_open(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode != LV_FS_MODE_RD) return nullptr;
  std::filesystem::path p = std::filesystem::path(g_data_root) /
                            (path[0] == '/' ? path + 1 : path);
  return std::fopen(p.string().c_str(), "rb");
}
lv_fs_res_t fs_close(lv_fs_drv_t*, void* fp) {
  if (fp) std::fclose(reinterpret_cast<std::FILE*>(fp));
  return LV_FS_RES_OK;
}
lv_fs_res_t fs_read(lv_fs_drv_t*, void* fp, void* buf, uint32_t n, uint32_t* br) {
  *br = (uint32_t)std::fread(buf, 1, n, reinterpret_cast<std::FILE*>(fp));
  return LV_FS_RES_OK;
}
lv_fs_res_t fs_seek(lv_fs_drv_t*, void* fp, uint32_t pos, lv_fs_whence_t whence) {
  int w = SEEK_SET;
  if (whence == LV_FS_SEEK_CUR) w = SEEK_CUR;
  else if (whence == LV_FS_SEEK_END) w = SEEK_END;
  std::fseek(reinterpret_cast<std::FILE*>(fp), pos, w);
  return LV_FS_RES_OK;
}
lv_fs_res_t fs_tell(lv_fs_drv_t*, void* fp, uint32_t* pos) {
  *pos = (uint32_t)std::ftell(reinterpret_cast<std::FILE*>(fp));
  return LV_FS_RES_OK;
}

lv_fs_drv_t g_fs_drv;

void init_fs_driver() {
  lv_fs_drv_init(&g_fs_drv);
  g_fs_drv.letter   = 'L';
  g_fs_drv.cache_size = 0;
  g_fs_drv.open_cb  = fs_open;
  g_fs_drv.close_cb = fs_close;
  g_fs_drv.read_cb  = fs_read;
  g_fs_drv.seek_cb  = fs_seek;
  g_fs_drv.tell_cb  = fs_tell;
  lv_fs_drv_register(&g_fs_drv);
}

void init_sdl() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return;
  }
  // Upscale 2x for legibility on a desktop monitor.
  g_window = SDL_CreateWindow("CYD Sim", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              g_w * 2, g_h * 2, SDL_WINDOW_SHOWN);
  g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565,
                                SDL_TEXTUREACCESS_STREAMING, g_w, g_h);
}

}  // namespace

void init(Mode mode, int width, int height, const std::string& data_root) {
  g_mode      = mode;
  g_w         = width;
  g_h         = height;
  g_data_root = data_root;
  g_fb.assign((size_t)width * height, 0);

  lv_init();
  lv_tick_set_cb(tick_cb);

  lv_display_t* disp = lv_display_create(width, height);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_set_buffers(disp, g_buf1, g_buf2, sizeof(g_buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* mouse = lv_indev_create();
  lv_indev_set_type(mouse, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(mouse, mouse_cb);

  init_fs_driver();

  if (mode == Mode::Window) init_sdl();
}

bool tick() {
  lv_timer_handler();

  if (g_mode == Mode::Window) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT: g_quit = true; break;
        case SDL_MOUSEBUTTONDOWN: g_mouse_down = true;  // fallthrough
        case SDL_MOUSEMOTION:     g_mouse_x = e.motion.x / 2; g_mouse_y = e.motion.y / 2; break;
        case SDL_MOUSEBUTTONUP:   g_mouse_down = false; break;
      }
    }
    if (g_texture) {
      SDL_UpdateTexture(g_texture, nullptr, g_fb.data(), g_w * 2);
      SDL_RenderClear(g_renderer);
      SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
      SDL_RenderPresent(g_renderer);
    }
    if (g_quit) return false;
  }
  return true;
}

void inject_click(int x, int y, int down_ms) {
  g_mouse_x = x;
  g_mouse_y = y;
  g_mouse_down = true;
  // Pump a few timer-handler frames so LVGL sees the press, the click event
  // propagates, and any modal dialogs open.
  int press_ticks = down_ms < 20 ? 1 : down_ms / 20;
  for (int i = 0; i < press_ticks + 1; ++i) {
    lv_timer_handler();
  }
  g_mouse_down = false;
  for (int i = 0; i < 4; ++i) {
    lv_timer_handler();
  }
}

bool dump_png(const std::string& path) {
  // Convert RGB565 -> RGB888 then write via stb_image_write.
  std::vector<uint8_t> rgb(g_w * g_h * 3);
  for (int i = 0; i < g_w * g_h; ++i) {
    uint16_t p = g_fb[i];
    uint8_t r = (p >> 11) & 0x1F;
    uint8_t g = (p >> 5)  & 0x3F;
    uint8_t b =  p        & 0x1F;
    rgb[i*3+0] = (r << 3) | (r >> 2);
    rgb[i*3+1] = (g << 2) | (g >> 4);
    rgb[i*3+2] = (b << 3) | (b >> 2);
  }
  return stbi_write_png(path.c_str(), g_w, g_h, 3, rgb.data(), g_w * 3) != 0;
}

void shutdown() {
  if (g_texture)  { SDL_DestroyTexture(g_texture);   g_texture  = nullptr; }
  if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = nullptr; }
  if (g_window)   { SDL_DestroyWindow(g_window);     g_window   = nullptr; }
  if (g_mode == Mode::Window) SDL_Quit();
}

}  // namespace sim_bridge
