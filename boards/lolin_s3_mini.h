/**
 * Board Configuration — LOLIN S3 Mini
 *
 * ⚠️ UNTESTED — Community board, not verified by maintainers.
 *
 * Compact Wemos/LOLIN board in a D1 Mini form factor.
 * Uses native USB-OTG for HID.
 *
 * Specs:
 *   - ESP32-S3 (BLE 5.0 + native USB OTG)
 *   - 4MB Flash, 2MB PSRAM
 *   - WS2812B RGB LED on GPIO 47 (requires NeoPixel library)
 *   - Boot button on GPIO 0
 *   - 27 digital I/O pins
 *
 * Note: The onboard LED is a WS2812B addressable RGB LED which
 * requires a NeoPixel library — simple digitalWrite won't work.
 * LED feedback is disabled for this board (LED_PIN = -1).
 *
 * Purchase: https://www.wemos.cc/en/latest/s3/s3_mini.html
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
#define BOARD_NAME "LOLIN S3 Mini"

// ─── Pin assignments ────────────────────────────────────────────────────────
// WS2812B LED on GPIO 47 (not usable with digitalWrite)
#define BOARD_LED_PIN -1
#define BOARD_BOOT_PIN 0

// ─── LED behavior ───────────────────────────────────────────────────────────
#define BOARD_LED_INVERTED false
