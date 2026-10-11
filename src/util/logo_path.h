#pragma once

#include <Arduino.h>

// The one place that maps a symbol to its cached logo file. The fetcher
// (writer) and the UI (reader) must agree byte for byte: they used to build
// the path separately and disagreed for any symbol with '-', '.' or '^'
// (BTC-USD, BRK.B, ^GSPC): the logo was downloaded as BTC_USD.png but looked
// up as BTC-USD.png, so it never showed and was never re-fetched.
//
// Upper-case A-Z/0-9 are kept, anything else becomes '_' (LittleFS-safe).
inline String logoPath(const String& symbol) {
  String s = symbol;
  s.toUpperCase();
  String clean;
  clean.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    clean += ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
  }
  return String("/logos/") + clean + ".png";
}
