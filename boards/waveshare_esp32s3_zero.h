/**
 * Board Configuration — Waveshare ESP32-S3-Zero
 *
 * ⚠️ UNTESTED — Community board, not verified by maintainers.
 *
 * Ultra-compact zero form factor ESP32-S3 board from Waveshare.
 * Uses native USB-OTG for HID. Very small footprint.
 *
 * Specs:
 *   - ESP32-S3 (BLE 5.0 + native USB OTG)
 *   - 4MB Flash
 *   - WS2812 RGB LED on GPIO 21 (requires NeoPixel library)
 *   - Boot button on GPIO 0
 *   - Castellated pads for direct soldering
 *
 * Note: The onboard LED is a WS2812 addressable RGB LED which
 * requires a NeoPixel library — simple digitalWrite won't work.
 * LED feedback is disabled for this board (LED_PIN = -1).
 *
 * Purchase: https://www.waveshare.com/esp32-s3-zero.htm
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
#define BOARD_NAME "Waveshare ESP32-S3-Zero"

// ─── Pin assignments ────────────────────────────────────────────────────────
// WS2812 LED on GPIO 21 (not usable with digitalWrite)
#define BOARD_LED_PIN -1
#define BOARD_BOOT_PIN 0

// ─── LED behavior ───────────────────────────────────────────────────────────
#define BOARD_LED_INVERTED false
