#include "quote_store.h"

void QuoteStore::begin() {
  _mu = xSemaphoreCreateMutex();
}

void QuoteStore::setQuotes(std::vector<Quote> q, time_t epoch) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _quotes = std::move(q);
  _lastUpdate = epoch;
  xSemaphoreGive(_mu);
}

std::vector<Quote> QuoteStore::snapshot() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  std::vector<Quote> copy = _quotes;
  xSemaphoreGive(_mu);
  return copy;
}

time_t QuoteStore::lastUpdate() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  time_t v = _lastUpdate;
  xSemaphoreGive(_mu);
  return v;
}

void QuoteStore::setHistory(History h) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _history = std::move(h);
  xSemaphoreGive(_mu);
}

History QuoteStore::history() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  History copy = _history;
  xSemaphoreGive(_mu);
  return copy;
}

void QuoteStore::requestHistory(const String& symbol) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _pendingHistory = symbol;
  xSemaphoreGive(_mu);
}

String QuoteStore::takePendingHistory() {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String s = _pendingHistory;
  _pendingHistory = "";
  xSemaphoreGive(_mu);
  return s;
}
