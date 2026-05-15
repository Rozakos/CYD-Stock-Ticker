#pragma once
// Recursive-mutex stand-in for FreeRTOS binary mutex.

#include <mutex>

#include "FreeRTOS.h"

using SemaphoreHandle_t = std::recursive_mutex*;

inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  return new std::recursive_mutex();
}
inline int xSemaphoreTake(SemaphoreHandle_t h, uint32_t /*ticks*/) {
  if (h) h->lock();
  return 1;
}
inline int xSemaphoreGive(SemaphoreHandle_t h) {
  if (h) h->unlock();
  return 1;
}
