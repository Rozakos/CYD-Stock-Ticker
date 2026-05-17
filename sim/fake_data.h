#pragma once
class QuoteStore;
namespace fake_data {
void seed(QuoteStore& store);
void seed_intraday(QuoteStore& store, const char* symbol);
}
