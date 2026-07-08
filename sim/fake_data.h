#pragma once
class QuoteStore;
namespace fake_data {
void seed(QuoteStore& store);
void seed_intraday(QuoteStore& store, const char* symbol);
// `gen` is the HistoryRequest generation being answered — the detail screen
// only renders a result whose gen matches its latest request.
void seed_range_history(QuoteStore& store, const char* symbol, const char* range,
                        unsigned gen = 0);
}
