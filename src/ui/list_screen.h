#pragma once

#include <lvgl.h>

class QuoteStore;

namespace list_screen {

void build(QuoteStore* store);
lv_obj_t* screen();
void tick();  // call from UI timer to refresh from store

}  // namespace list_screen
