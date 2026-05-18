#pragma once
class QuoteStore;
namespace fake_data {
void seed(QuoteStore& store);
void seed_intraday(QuoteStore& store, const char* symbol);
void seed_range_history(QuoteStore& store, const char* symbol, const char* range);
}
