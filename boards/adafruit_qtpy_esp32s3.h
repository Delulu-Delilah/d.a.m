/**
 * Board Configuration — Adafruit QT Py ESP32-S3
 *
 * ⚠️ UNTESTED — Community board, not verified by maintainers.
 *
 * Compact Adafruit QT Py form factor with STEMMA QT connector.
 * Uses native USB-OTG for HID. Available in two flash/PSRAM
 * configurations (4MB+2MB PSRAM or 8MB no PSRAM).
 *
 * Specs:
 *   - ESP32-S3 (BLE 5.0 + native USB OTG)
 *   - 4MB Flash + 2MB PSRAM  (or 8MB Flash, no PSRAM)
 *   - NeoPixel RGB LED (requires NeoPixel library, not simple GPIO)
 *   - Boot button on GPIO 0
 *   - 17.5×22.8mm form factor
 *
 * Note: The onboard LED is a NeoPixel which requires a library —
 * simple digitalWrite won't work.
 * LED feedback is disabled for this board (LED_PIN = -1).
 *
 * Purchase: https://www.adafruit.com/product/5426
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
#define BOARD_NAME "Adafruit QT Py ESP32-S3"

// ─── Pin assignments ────────────────────────────────────────────────────────
// NeoPixel LED (not usable with digitalWrite)
#define BOARD_LED_PIN -1
#define BOARD_BOOT_PIN 0

// ─── LED behavior ───────────────────────────────────────────────────────────
#define BOARD_LED_INVERTED false
