#include "fs_littlefs.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <lvgl.h>

namespace {

struct Handle {
  File file;
};

void* lv_open(lv_fs_drv_t*, const char* path, lv_fs_mode_t mode) {
  const char* fmode = (mode & LV_FS_MODE_WR) ? "r+" : "r";
  File f = LittleFS.open(path, fmode);
  if (!f) return nullptr;
  auto* h = new Handle{std::move(f)};
  return h;
}

lv_fs_res_t lv_close(lv_fs_drv_t*, void* file_p) {
  auto* h = static_cast<Handle*>(file_p);
  h->file.close();
  delete h;
  return LV_FS_RES_OK;
}

lv_fs_res_t lv_read(lv_fs_drv_t*, void* file_p, void* buf, uint32_t btr,
                    uint32_t* br) {
  auto* h = static_cast<Handle*>(file_p);
  *br = h->file.read(static_cast<uint8_t*>(buf), btr);
  return LV_FS_RES_OK;
}

lv_fs_res_t lv_seek(lv_fs_drv_t*, void* file_p, uint32_t pos,
                    lv_fs_whence_t whence) {
  auto* h = static_cast<Handle*>(file_p);
  SeekMode m = SeekSet;
  if (whence == LV_FS_SEEK_CUR) m = SeekCur;
  else if (whence == LV_FS_SEEK_END) m = SeekEnd;
  return h->file.seek(pos, m) ? LV_FS_RES_OK : LV_FS_RES_UNKNOWN;
}

lv_fs_res_t lv_tell(lv_fs_drv_t*, void* file_p, uint32_t* pos_p) {
  auto* h = static_cast<Handle*>(file_p);
  *pos_p = h->file.position();
  return LV_FS_RES_OK;
}

lv_fs_drv_t g_drv;

}  // namespace

namespace fs_littlefs {

void init() {
  lv_fs_drv_init(&g_drv);
  g_drv.letter   = 'L';
  g_drv.open_cb  = lv_open;
  g_drv.close_cb = lv_close;
  g_drv.read_cb  = lv_read;
  g_drv.seek_cb  = lv_seek;
  g_drv.tell_cb  = lv_tell;
  lv_fs_drv_register(&g_drv);
}

}  // namespace fs_littlefs
