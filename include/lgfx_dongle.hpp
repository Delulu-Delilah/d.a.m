// LovyanGFX: ST7735 0.96" 160×80 (SPI) — ESP32-S3 dongle clone pinout:
//   SCLK=10, MOSI=11, CS=12, DC=13, RST=14, SPI2_HOST

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <driver/spi_common.h>

class LGFX_Dongle : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX_Dongle() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 20000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 10;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 13;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = 12;
      cfg.pin_rst = 14;
      cfg.pin_busy = -1;
      cfg.memory_width = 132;
      cfg.memory_height = 162;
      cfg.panel_width = 80;
      cfg.panel_height = 160;
      cfg.offset_x = 26;
      cfg.offset_y = 1;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
