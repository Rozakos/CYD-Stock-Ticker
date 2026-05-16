"""One-off helper: download famous-ticker logos and resize for the device.

Source: https://github.com/nvstly/icons (MIT, white-foreground on transparent
for most US tickers; some are full-color brand marks). Output: 48x48 RGBA
PNGs in data/logos/ — small enough that ~25 tickers fit comfortably in the
128 KB LittleFS partition with 4 KB block overhead.

Run from the project root:  python sim/fetch_logos.py
"""

import io
import os
import sys
import urllib.request
from PIL import Image

TICKERS = [
    # Big tech
    "AAPL", "MSFT", "NVDA", "GOOG", "GOOGL", "AMZN", "META", "NFLX", "TSLA",
    # Semis
    "AMD", "INTC", "AVGO", "TSM", "QCOM",
    # Software & cloud
    "ADBE", "ORCL", "CRM",
    # Finance
    "JPM", "V", "MA", "BAC",
    # Other mega-caps
    "JNJ", "WMT", "DIS", "KO", "PEP", "COST", "XOM", "BA", "IBM",
]

BASE = "https://raw.githubusercontent.com/nvstly/icons/main/ticker_icons/{0}.png"
OUT  = "data/logos"
SIZE = 48

def download(ticker: str) -> bytes | None:
    url = BASE.format(ticker)
    req = urllib.request.Request(url, headers={"User-Agent": "stock-ticker-fetcher/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            if r.status != 200:
                print(f"  [{ticker}] HTTP {r.status}", file=sys.stderr)
                return None
            return r.read()
    except Exception as e:
        print(f"  [{ticker}] {e}", file=sys.stderr)
        return None

def resize(blob: bytes) -> bytes:
    img = Image.open(io.BytesIO(blob)).convert("RGBA")
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    out = io.BytesIO()
    img.save(out, "PNG", optimize=True)
    return out.getvalue()

def main():
    os.makedirs(OUT, exist_ok=True)
    # Clean the existing set first so we know exactly what's on disk.
    for name in os.listdir(OUT):
        if name.lower().endswith(".png"):
            os.remove(os.path.join(OUT, name))

    ok, total = 0, 0
    for t in TICKERS:
        blob = download(t)
        if not blob:
            continue
        small = resize(blob)
        path = os.path.join(OUT, f"{t}.png")
        with open(path, "wb") as f:
            f.write(small)
        ok += 1
        total += len(small)
        print(f"  [{t}] {len(small)} B")
    print(f"\n{ok}/{len(TICKERS)} tickers, total {total} bytes ({total/1024:.1f} KB)")

if __name__ == "__main__":
    main()
