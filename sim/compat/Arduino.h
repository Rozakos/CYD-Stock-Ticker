#pragma once

// Minimal Arduino-ESP32 compatibility shim for the LVGL desktop simulator.
// Only covers the surface actually used by code we link into the sim
// (ui/, net/quote_store, settings/settings_store). Keep it small.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#ifndef NAN
#  define NAN __builtin_nanf("")
#endif

// ---- Arduino-style String --------------------------------------------------
// Subset of WString.h sufficient for this project. Backed by std::string.
class String {
 public:
  String() = default;
  String(const char* s) : _s(s ? s : "") {}
  String(const std::string& s) : _s(s) {}
  String(char c) : _s(1, c) {}
  String(int v)          { char b[16]; std::snprintf(b, sizeof b, "%d",   v); _s = b; }
  String(long v)         { char b[24]; std::snprintf(b, sizeof b, "%ld",  v); _s = b; }
  String(unsigned v)     { char b[16]; std::snprintf(b, sizeof b, "%u",   v); _s = b; }
  String(unsigned long v){ char b[24]; std::snprintf(b, sizeof b, "%lu",  v); _s = b; }
  String(float v, int decimals = 2) {
    char b[32]; std::snprintf(b, sizeof b, "%.*f", decimals, v); _s = b;
  }
  String(double v, int decimals = 2) {
    char b[32]; std::snprintf(b, sizeof b, "%.*f", decimals, v); _s = b;
  }

  const char* c_str() const { return _s.c_str(); }
  size_t      length() const { return _s.size(); }
  bool        isEmpty() const { return _s.empty(); }
  char        operator[](size_t i) const { return _s[i]; }
  char        charAt(size_t i)     const { return _s[i]; }

  String& operator=(const char* s) { _s = s ? s : ""; return *this; }
  String& operator=(const std::string& s) { _s = s; return *this; }

  String& operator+=(const String& o) { _s += o._s; return *this; }
  String& operator+=(const char* s)   { _s += (s ? s : ""); return *this; }
  String& operator+=(char c)          { _s += c; return *this; }

  bool operator==(const String& o) const { return _s == o._s; }
  bool operator!=(const String& o) const { return _s != o._s; }
  bool operator==(const char* s)   const { return _s == (s ? s : ""); }
  bool operator!=(const char* s)   const { return _s != (s ? s : ""); }

  int indexOf(char c, size_t from = 0) const {
    auto p = _s.find(c, from);
    return p == std::string::npos ? -1 : static_cast<int>(p);
  }
  int indexOf(const char* needle, size_t from = 0) const {
    auto p = _s.find(needle, from);
    return p == std::string::npos ? -1 : static_cast<int>(p);
  }

  String substring(int start) const {
    if (start < 0) start = 0;
    if ((size_t)start >= _s.size()) return String();
    return String(_s.substr(start));
  }
  String substring(int start, int end) const {
    if (start < 0) start = 0;
    if ((size_t)start >= _s.size()) return String();
    if (end < start) end = start;
    if ((size_t)end > _s.size()) end = (int)_s.size();
    return String(_s.substr(start, end - start));
  }

  void trim() {
    auto a = _s.find_first_not_of(" \t\r\n");
    auto b = _s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) { _s.clear(); return; }
    _s = _s.substr(a, b - a + 1);
  }
  void toUpperCase() {
    for (auto& c : _s) c = (char)std::toupper((unsigned char)c);
  }
  void toLowerCase() {
    for (auto& c : _s) c = (char)std::tolower((unsigned char)c);
  }

  float toFloat() const { return _s.empty() ? 0.f : (float)std::atof(_s.c_str()); }
  long  toInt()   const { return _s.empty() ? 0L  : std::atol(_s.c_str()); }

  bool equalsIgnoreCase(const char* other) const {
    if (!other) return _s.empty();
    size_t i = 0;
    for (; i < _s.size() && other[i]; ++i) {
      if (std::tolower((unsigned char)_s[i]) != std::tolower((unsigned char)other[i])) return false;
    }
    return i == _s.size() && other[i] == 0;
  }
  bool equalsIgnoreCase(const String& other) const { return equalsIgnoreCase(other.c_str()); }

  // ArduinoJson stream API: returns truthy on success.
  unsigned char concat(const char* s) { if (s) _s += s; return 1; }
  unsigned char concat(char c)        { _s += c; return 1; }
  unsigned char concat(const String& s) { _s += s._s; return 1; }

  // Used by ArduinoJson glue.
  const std::string& std_str() const { return _s; }

 private:
  std::string _s;
};

inline String operator+(String a, const String& b) { a += b; return a; }
inline String operator+(String a, const char*   b) { a += b; return a; }
inline String operator+(const char* a, const String& b) { String r(a); r += b; return r; }

// ---- Time / scheduling ----------------------------------------------------
uint32_t millis();
uint32_t micros();
void     delay(uint32_t ms);
void     delayMicroseconds(uint32_t us);

// ---- Logging --------------------------------------------------------------
void sim_log(char level, const char* fmt, ...);
#define log_i(fmt, ...) sim_log('I', fmt, ##__VA_ARGS__)
#define log_w(fmt, ...) sim_log('W', fmt, ##__VA_ARGS__)
#define log_e(fmt, ...) sim_log('E', fmt, ##__VA_ARGS__)
#define log_d(fmt, ...) sim_log('D', fmt, ##__VA_ARGS__)

// ---- localtime_r polyfill (MinGW only ships localtime_s) ------------------
#if defined(_WIN32) && !defined(localtime_r)
inline struct tm* localtime_r(const time_t* t, struct tm* out) {
  if (!t || !out) return nullptr;
  return localtime_s(out, t) == 0 ? out : nullptr;
}
#endif

// ---- Misc Arduino constants ----------------------------------------------
#ifndef HIGH
#  define HIGH 1
#  define LOW  0
#endif
#ifndef INPUT
#  define INPUT  0
#  define OUTPUT 1
#endif

// ---- Compatibility no-ops for ESP32-only attributes ----------------------
#ifndef DMA_ATTR
#  define DMA_ATTR
#endif
#ifndef IRAM_ATTR
#  define IRAM_ATTR
#endif

// ---- Serial stub ----------------------------------------------------------
class SerialStub {
 public:
  void begin(unsigned long) {}
  void print(const char* s)    { std::fputs(s ? s : "", stdout); }
  void print(const String& s)  { std::fputs(s.c_str(),  stdout); }
  void println()               { std::fputc('\n', stdout); }
  void println(const char* s)  { print(s); println(); }
  void println(const String& s){ print(s); println(); }
  void flush() { std::fflush(stdout); }
};
extern SerialStub Serial;
