Drop one PNG per stock symbol here, named UPPERCASE:

    AAPL.png
    MSFT.png
    NVDA.png
    ...

Recommended size: 48x48 or 64x64, transparent background.
The LVGL lodepng decoder accepts standard 8-bit RGBA PNGs.
Keep each file under ~4 KB to be kind to flash and decode time.

Upload to the board with:
    pio run -t uploadfs

Symbols without a PNG fall back to a colored letter badge
(brand color + first 1-2 characters of the ticker), so this
folder can stay empty if you don't want to bundle art.
