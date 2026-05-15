#pragma once
// Maps LittleFS calls to a host-side scratch directory (sim_data/).

#include <cstdio>
#include <filesystem>

#include "Arduino.h"

class File {
 public:
  File() = default;
  explicit File(std::FILE* fp) : _fp(fp) {}
  ~File() { close(); }

  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& o) noexcept : _fp(o._fp) { o._fp = nullptr; }
  File& operator=(File&& o) noexcept {
    if (this != &o) { close(); _fp = o._fp; o._fp = nullptr; }
    return *this;
  }

  explicit operator bool() const { return _fp != nullptr; }
  bool operator!() const { return _fp == nullptr; }

  void close() {
    if (_fp) { std::fclose(_fp); _fp = nullptr; }
  }

  size_t write(const uint8_t* buf, size_t len) {
    return _fp ? std::fwrite(buf, 1, len, _fp) : 0;
  }
  size_t write(uint8_t b) { return _fp ? std::fwrite(&b, 1, 1, _fp) : 0; }
  size_t print(const char* s) {
    return _fp && s ? std::fwrite(s, 1, std::strlen(s), _fp) : 0;
  }
  size_t print(const String& s) { return print(s.c_str()); }
  size_t println(const char* s) { return print(s) + print("\n"); }
  size_t println(const String& s) { return print(s) + print("\n"); }

  int read() { return _fp ? std::fgetc(_fp) : -1; }
  size_t readBytes(char* buf, size_t len) {
    return _fp ? std::fread(buf, 1, len, _fp) : 0;
  }
  String readString() {
    if (!_fp) return String();
    std::string out;
    int c;
    while ((c = std::fgetc(_fp)) != EOF) out.push_back((char)c);
    return String(out);
  }
  size_t size() {
    if (!_fp) return 0;
    long here = std::ftell(_fp);
    std::fseek(_fp, 0, SEEK_END);
    long end = std::ftell(_fp);
    std::fseek(_fp, here, SEEK_SET);
    return end > 0 ? (size_t)end : 0;
  }

  // ArduinoJson stream API.
  int available() {
    if (!_fp) return 0;
    long here = std::ftell(_fp);
    std::fseek(_fp, 0, SEEK_END);
    long end = std::ftell(_fp);
    std::fseek(_fp, here, SEEK_SET);
    return (int)(end - here);
  }
  int peek() {
    if (!_fp) return -1;
    int c = std::fgetc(_fp);
    if (c != EOF) std::ungetc(c, _fp);
    return c;
  }

 private:
  std::FILE* _fp = nullptr;
};

class LittleFSStub {
 public:
  // Writable root (settings). Created on begin().
  static constexpr const char* kRoot = "sim_data";
  // Optional read-only overlay searched for files missing from kRoot
  // (lets the sim pick up data/logos/ without copying).
  std::string read_overlay;

  bool begin(bool /*format_if_failed*/ = false) {
    std::error_code ec;
    std::filesystem::create_directories(kRoot, ec);
    return true;
  }

  bool exists(const char* path) {
    if (std::filesystem::exists(write_path(path))) return true;
    if (!read_overlay.empty()) return std::filesystem::exists(overlay_path(path));
    return false;
  }
  bool exists(const String& path) { return exists(path.c_str()); }

  File open(const String& path, const char* mode) { return open(path.c_str(), mode); }
  File open(const char* path, const char* mode) {
    bool writing = std::strchr(mode, 'w') != nullptr;
    auto host = write_path(path);
    if (writing) {
      auto parent = host.parent_path();
      if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
      }
      return File(std::fopen(host.string().c_str(), mode));
    }
    if (auto* fp = std::fopen(host.string().c_str(), mode)) return File(fp);
    if (!read_overlay.empty()) {
      auto over = overlay_path(path);
      return File(std::fopen(over.string().c_str(), mode));
    }
    return File();
  }

 private:
  static std::filesystem::path strip_leading_slash(const char* p) {
    std::string s = p ? p : "";
    if (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
  }
  static std::filesystem::path write_path(const char* p) {
    return std::filesystem::path(kRoot) / strip_leading_slash(p);
  }
  std::filesystem::path overlay_path(const char* p) const {
    return std::filesystem::path(read_overlay) / strip_leading_slash(p);
  }
};

extern LittleFSStub LittleFS;
