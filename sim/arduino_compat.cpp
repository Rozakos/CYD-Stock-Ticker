#include "Arduino.h"
#include "WiFi.h"
#include "LittleFS.h"

#include <chrono>
#include <thread>

namespace {
const auto kBoot = std::chrono::steady_clock::now();
}

uint32_t millis() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - kBoot).count();
}
uint32_t micros() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<microseconds>(steady_clock::now() - kBoot).count();
}
void delay(uint32_t ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
void delayMicroseconds(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void sim_log(char level, const char* fmt, ...) {
  std::fprintf(stderr, "[%c] ", level);
  std::va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

SerialStub   Serial;
WiFiStub     WiFi;
LittleFSStub LittleFS;
