Drop one PNG per stock symbol here, named UPPERCASE:

    AAPL.png
    MSFT.png
    NVDA.png
    ...

Recommended size: 48x48, transparent background.
The LVGL lodepng decoder accepts standard 8-bit RGBA PNGs.
Keep each file under ~4 KB to be kind to flash and decode time.

Upload to the board with:
    pio run -t uploadfs

Symbols without a PNG fall back to a colored letter badge
(brand color + first 1-2 characters of the ticker), so this
folder can stay empty if you don't want to bundle art.

----------------------------------------------------------------
A starter set of 50 logos (38 stocks + 12 cryptos) is shipped in
this folder, resized to 48x48 from:

  Stocks  : https://github.com/nvstly/icons (MIT)
            https://github.com/davidepalazzo/ticker-logos (MIT)
  Cryptos : https://github.com/spothq/cryptocurrency-icons (CC0)

Logos remain trademarks of their respective owners; they're
included here for personal/educational use on this device.
Replace or remove any you don't want.
