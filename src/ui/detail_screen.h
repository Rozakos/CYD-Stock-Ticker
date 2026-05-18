#pragma once

#include <Arduino.h>
#include <lvgl.h>

class QuoteStore;

namespace detail_screen {

void show(QuoteStore* store, const String& symbol);
void tick();  // poll history slot for newly arrived data
bool active();

}  // namespace detail_screen
