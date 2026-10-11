#pragma once

class SettingsStore;
class QuoteStore;

namespace web_admin {

// `quotes` feeds GET /api/state (live prices for the Rozakos Home app).
void begin(SettingsStore* settings, QuoteStore* quotes = nullptr);

}  // namespace web_admin
