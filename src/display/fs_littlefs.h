#pragma once

namespace fs_littlefs {

// Registers an LVGL filesystem driver under drive letter 'L'. After this
// returns, lv_image / decoders can open files via paths like
// "L:/logos/AAPL.png".
void init();

// Serialize all LittleFS access across tasks. LittleFS is not safe under
// concurrent access from multiple FreeRTOS tasks — the LVGL FS driver
// (UI task, decoding logo PNGs) and SettingsStore::save/load (AsyncTCP
// worker on a POST, or boot-time net task) must take this lock around
// every LittleFS call. Failing to do so manifests as an lfs assertion
// crash inside lfs_file_read / lfs_mlist_isopen.
void lock();
void unlock();

class Guard {
 public:
  Guard()  { lock(); }
  ~Guard() { unlock(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

}  // namespace fs_littlefs
