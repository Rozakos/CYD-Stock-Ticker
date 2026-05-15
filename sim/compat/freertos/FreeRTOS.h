#pragma once
// Empty stub — semphr.h pulls in everything needed for the sim.

#include <cstdint>

#define portMAX_DELAY  ((uint32_t)0xFFFFFFFF)
#define pdMS_TO_TICKS(ms) (ms)

inline void vTaskDelay(uint32_t /*ticks*/) {}
inline void vTaskDelete(void*) {}
