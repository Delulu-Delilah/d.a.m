/**
 * Board Configuration — ESP32-S3 USB dongle with ST7735 160×80 (T-Dongle class)
 *
 * Used with AliExpress / LilyGO-style dongles: SPI TFT on GPIO 10–14 (see
 * include/lgfx_dongle.hpp). No IMU on-board — orientation comes from the
 * Daydream controller over BLE (same as other Daydream Air Mouse targets).
 *
 * LED: many clones route a status or RGB LED differently; set to -1 if your
 * board has no safe GPIO for the stock LED helpers, or set BOARD_LED_PIN to
 * match your schematic (e.g. 38 on some LilyGO variants).
 */

#pragma once

#define BOARD_NAME "ESP32-S3 T-Dongle (ST7735)"

// No dedicated status LED on some clones — avoid driving wrong pin.
#define BOARD_LED_PIN -1

#define BOARD_BOOT_PIN 0

#define BOARD_LED_INVERTED true
