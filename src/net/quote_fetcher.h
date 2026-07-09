#pragma once

#include <Arduino.h>

class SettingsStore;
class QuoteStore;

namespace fetcher {

bool fetchQuotes(SettingsStore& settings, QuoteStore& store);

// Pulls /history/{symbol}?range={range}. The response carries both the
// points array AND a top-level "interval" string ("intraday" or "daily")
// that drives the detail-screen X-axis label formatter. `gen` is the
// generation ID the caller got back from QuoteStore::requestHistory —
// the result is only stored if it's still current at the end of the
// fetch (so a later requestHistory call cancels this one).
bool fetchHistory(SettingsStore& settings, QuoteStore& store,
                  const String& symbol, const String& range,
                  uint32_t gen);

// True when at least one HTTP request during the most recent fetchQuotes
// cycle got a real server response (any status > 0, including 401/5xx).
// False means every attempt died at the transport layer — the signal the
// net task's watchdog uses to tell a wedged TCP stack (reboot-worthy) from
// a server-side problem like a bad token (never reboot-worthy).
bool sawHttpResponseThisCycle();

// Drops the persistent API connection so its TLS buffers (~40 KB resident)
// return to the heap. The net task calls this when the UI reports a runtime
// logo decode was starved for contiguous heap (logos::consumeDecodeStarved);
// the next fetch reconnects transparently. No-op when already disconnected.
// Must be called from the net task — it owns the connection.
void releaseApiConnection();

// One-time cache invalidation. If the version stored in /logos/.cachever
// differs from `version`, delete every cached /logos/*.png (so they re-download
// at the current server quality/size) and record the new version. Cheap no-op
// once the versions match. Call once at boot, after LittleFS is mounted.
void purgeStaleLogoCache(uint32_t version);

}  // namespace fetcher
