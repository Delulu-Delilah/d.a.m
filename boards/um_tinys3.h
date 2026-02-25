/**
 * Board Configuration — Unexpected Maker TinyS3
 *
 * ⚠️ UNTESTED — Community board, not verified by maintainers.
 *
 * Ultra-compact ESP32-S3 board from Unexpected Maker.
 * Uses native USB-OTG for HID. Features battery management.
 *
 * Specs:
 *   - ESP32-S3 (BLE 5.0 + native USB OTG)
 *   - 8MB Flash, 8MB PSRAM
 *   - WS2812 RGB LED on GPIO 18 (requires NeoPixel library)
 *   - Boot button on GPIO 0
 *   - 17 GPIO pins broken out
 *
 * Note: The onboard LED is a WS2812 addressable RGB LED which
 * requires a NeoPixel library — simple digitalWrite won't work.
 * LED feedback is disabled for this board (LED_PIN = -1).
 *
 * Purchase: https://unexpectedmaker.com/shop/tinys3
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
#define BOARD_NAME "Unexpected Maker TinyS3"

// ─── Pin assignments ────────────────────────────────────────────────────────
// WS2812 LED on GPIO 18 (not usable with digitalWrite)
#define BOARD_LED_PIN -1
#define BOARD_BOOT_PIN 0

// ─── LED behavior ───────────────────────────────────────────────────────────
#define BOARD_LED_INVERTED false
