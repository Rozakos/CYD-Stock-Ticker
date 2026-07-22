# Prompt for the stock-api side: add crypto support

We have an ESP32 stock-ticker firmware that consumes our yfinance proxy at
`https://rozakos.eu/stocks/api/v1`. I want to add **crypto** support. The
firmware is a fixed client — the goal is for crypto symbols to "just work"
through the EXISTING contract below **without changing the equities behavior**
and without a firmware change.

## Existing endpoints the device calls

All requests send `Authorization: Bearer <token>` and a non-empty `User-Agent`.
Responses must send `Content-Length` and identity (non-chunked) encoding.

**1) List** — `GET /stocks?symbols=A,B,...` (batches of up to 16; matched by
symbol case-insensitively; the server MUST echo back the same symbol string that
was requested):

```json
{ "quotes": [ {
    "symbol": "...",
    "last": 0,
    "change_pct": 0,
    "closes": [0, 0, 0],
    "market_state": "REGULAR|PRE|POST|CLOSED",
    "pre_market": null,  "pre_market_change_pct": null,
    "post_market": null, "post_market_change_pct": null
} ] }
```

**2) History** — `GET /history/{symbol}?range=<r>&limit=<n>`,
`r ∈ {1d,1w,1mo,6mo,1y,5y,max}`:

```json
{ "interval": "intraday|daily",
  "session_open": 0,    // 1d only
  "session_close": 0,   // 1d only
  "points": [ {"ts": 0, "last": 0} ] }
```

`limit` MUST be applied for **every** range (including `max`) — the ESP32 can't
parse large payloads. `ts` is epoch seconds UTC.

**3) Logo** — `GET /logo/{symbol}?size=48` → a COMPLETE 48x48 PNG (valid IEND
chunk), ≤ 64 KB. The device downloads and caches this; no icons are bundled in
firmware. Return **404** if no icon is available — the device falls back to a
letter badge.

## Crypto requirements

- **Symbol convention:** accept yfinance-style crypto pairs, e.g. `BTC-USD`,
  `ETH-USD`, `SOL-USD`. Echo the symbol back verbatim in `quotes[].symbol`.
  (Optionally also accept bare `BTC`/`ETH` and normalize to `-USD`, but the
  echoed symbol must match what was requested so the client maps it to the
  right row.)

- **`/stocks` for crypto:** crypto trades 24/7 with no pre/post sessions.
  ALWAYS set `market_state="REGULAR"` and leave `pre_market*`/`post_market*`
  null, so the client shows no after-hours indicator. `change_pct` = standard
  24h/daily change, consistent with the `closes[]` series. Populate `closes[]`
  with recent intraday closes just like equities.

- **`/history` for crypto:** 24/7 means `range=1d` covers a rolling 24h window.
  For crypto you MUST set `session_open`/`session_close` to that 24h window
  (e.g. `now-24h .. now`); otherwise the client assumes a 6.5h equity session
  and the 1d chart renders compressed/wrong. `interval`/`points` behave the same
  as equities; keep honoring `limit`.

- **`/logo` for crypto:** yfinance has no crypto icons, so this needs a separate
  source — see Data sources below. Still return a complete 48x48 PNG with
  `Content-Length`, or 404.

- **Caching:** equities `/stocks` is cached ~10 min. Crypto is volatile and
  24/7 — use a much shorter TTL (e.g. 30–60 s) for crypto symbols.

- Auth, headers, batching, and all field names stay exactly as above.

## Data sources

The firmware is source-agnostic; fill the fields from wherever is easiest.

- **Quotes + history:** yfinance natively supports major crypto pairs
  (`BTC-USD`, `ETH-USD`, …) including intraday and historical data, so the
  simplest path is to **keep using yfinance** for `/stocks` + `/history` for the
  major coins — no new integration. Caveat: coverage thins for small-cap
  altcoins and the 24h-change is tied to Yahoo's daily roll.
- **Logos:** yfinance has none. Add a crypto icon source — a bundled
  `cryptocurrency-icons` package, or CoinGecko's coin images keyed by base asset
  (`BTC-USD` → bitcoin).
- **Alternative (broader coverage + real 24/7 + free logos):** route crypto
  symbols to **CoinGecko** end-to-end (price, 24h change, market-chart history,
  AND coin image URLs in one API), leaving equities on yfinance. A single
  `if symbol is crypto → CoinGecko` branch covers quotes, history, and logos.

## Acceptance

- `GET /stocks?symbols=BTC-USD,ETH-USD,AAPL` returns all three; crypto entries
  have `market_state="REGULAR"` and null pre/post fields; AAPL unchanged.
- `GET /history/BTC-USD?range=1d&limit=30` returns `interval="intraday"`, 30
  points, and `session_open`/`session_close` spanning ~24h.
- `GET /logo/BTC-USD?size=48` returns a valid 48x48 PNG (or 404 with a letter
  badge fallback on device).
