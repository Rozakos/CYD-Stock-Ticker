#pragma once

namespace fs_littlefs {

// Registers an LVGL filesystem driver under drive letter 'L'. After this
// returns, lv_image / decoders can open files via paths like
// "L:/logos/AAPL.png".
void init();

}  // namespace fs_littlefs
