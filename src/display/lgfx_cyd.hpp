#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ESP32-2432S028R v2/v3 (dual-USB) "Cheap Yellow Display" — ST7789 on HSPI,
// XPT2046 on VSPI.
// TFT:   MOSI=13 MISO=12 SCLK=14 CS=15 DC=2  RST=-1 BL=21
// Touch: MOSI=32 MISO=39 SCLK=25 CS=33 IRQ=36
class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789   _panel;
  lgfx::Bus_SPI        _bus_tft;
  lgfx::Light_PWM      _light;
  lgfx::Touch_XPT2046  _touch;

 public:
  LGFX_CYD() {
    {
      auto cfg = _bus_tft.config();
      cfg.spi_host    = HSPI_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = 14;
      cfg.pin_mosi    = 13;
      cfg.pin_miso    = 12;
      cfg.pin_dc      = 2;
      _bus_tft.config(cfg);
      _panel.setBus(&_bus_tft);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs           = 15;
      cfg.pin_rst          = -1;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.readable         = false;
      cfg.invert           = true;
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    {
      // Touch_XPT2046 owns its own SPI config in this LovyanGFX version.
      auto cfg = _touch.config();
      cfg.spi_host        = VSPI_HOST;
      cfg.freq            = 1000000;
      cfg.bus_shared      = false;
      cfg.offset_rotation = 0;
      // Touch was mirrored 180° from the display in landscape (tapping the
      // top-right corner registered at the bottom-right and vice versa).
      // Swap min/max on both axes to invert them.
      cfg.x_min           = 3900;
      cfg.x_max           = 300;
      cfg.y_min           = 3850;
      cfg.y_max           = 200;
      cfg.pin_int         = 36;
      cfg.pin_cs          = 33;
      cfg.pin_sclk        = 25;
      cfg.pin_mosi        = 32;
      cfg.pin_miso        = 39;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};
