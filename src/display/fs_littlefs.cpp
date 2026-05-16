#include "fs_littlefs.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>

#include <cstdlib>
#include <cstring>

namespace {

SemaphoreHandle_t g_mu = nullptr;

// LVGL's PNG/image decoder opens a file, reads bits over many calls, and
// keeps it open while the image stays in its cache. Holding a LittleFS
// handle across that lifetime races with any other task that touches
// LittleFS (settings save, web_admin) and trips lfs_mlist asserts.
//
// We sidestep the whole problem: at lv_open we slurp the entire file into
// a malloc'd RAM buffer under the LittleFS mutex, close the file before
// returning, and serve every subsequent read/seek/tell from the buffer.
// LittleFS is only touched for ~one ms per image, and only ever from one
// task at a time.
struct Handle {
  uint8_t* data;
  size_t   size;
  size_t   pos;
};

void* lv_open(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  if (mode & LV_FS_MODE_WR) return nullptr;   // read-only
  fs_littlefs::Guard g;
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  size_t sz = f.size();
  uint8_t* buf = static_cast<uint8_t*>(std::malloc(sz));
  if (!buf) { f.close(); return nullptr; }
  size_t got = f.read(buf, sz);
  f.close();
  if (got != sz) { std::free(buf); return nullptr; }
  return new Handle{buf, sz, 0};
}

lv_fs_res_t lv_close(lv_fs_drv_t*, void* file_p) {
  auto* h = static_cast<Handle*>(file_p);
  if (h) {
    std::free(h->data);
    delete h;
  }
  return LV_FS_RES_OK;
}

lv_fs_res_t lv_read(lv_fs_drv_t*, void* file_p, void* buf, uint32_t btr,
                    uint32_t* br) {
  auto* h = static_cast<Handle*>(file_p);
  size_t remaining = (h->pos < h->size) ? (h->size - h->pos) : 0;
  size_t take = remaining < btr ? remaining : btr;
  std::memcpy(buf, h->data + h->pos, take);
  h->pos += take;
  *br = (uint32_t)take;
  return LV_FS_RES_OK;
}

lv_fs_res_t lv_seek(lv_fs_drv_t*, void* file_p, uint32_t pos,
                    lv_fs_whence_t whence) {
  auto* h = static_cast<Handle*>(file_p);
  size_t target;
  if (whence == LV_FS_SEEK_SET)      target = pos;
  else if (whence == LV_FS_SEEK_CUR) target = h->pos + pos;
  else                               target = h->size + pos;
  if (target > h->size) target = h->size;
  h->pos = target;
  return LV_FS_RES_OK;
}

lv_fs_res_t lv_tell(lv_fs_drv_t*, void* file_p, uint32_t* pos_p) {
  auto* h = static_cast<Handle*>(file_p);
  *pos_p = (uint32_t)h->pos;
  return LV_FS_RES_OK;
}

lv_fs_drv_t g_drv;

}  // namespace

namespace fs_littlefs {

void init() {
  if (!g_mu) g_mu = xSemaphoreCreateRecursiveMutex();
  lv_fs_drv_init(&g_drv);
  g_drv.letter   = 'L';
  g_drv.open_cb  = lv_open;
  g_drv.close_cb = lv_close;
  g_drv.read_cb  = lv_read;
  g_drv.seek_cb  = lv_seek;
  g_drv.tell_cb  = lv_tell;
  lv_fs_drv_register(&g_drv);
}

void lock() {
  if (!g_mu) g_mu = xSemaphoreCreateRecursiveMutex();
  xSemaphoreTakeRecursive(g_mu, portMAX_DELAY);
}

void unlock() {
  if (g_mu) xSemaphoreGiveRecursive(g_mu);
}

}  // namespace fs_littlefs
