/**
 * Board Configuration — ESP32-S3-DevKitC-1
 *
 * ⚠️ UNTESTED — Community board, not verified by maintainers.
 *
 * Espressif's official general-purpose ESP32-S3 development board.
 * Dual USB-C connectors (USB-OTG + USB-UART). Use the USB-OTG port
 * for HID functionality.
 *
 * Specs:
 *   - ESP32-S3 (BLE 5.0 + native USB OTG)
 *   - 8MB Flash (N8), optional PSRAM variants
 *   - Addressable RGB LED on GPIO 48 (rev 1.0) or GPIO 38 (rev 1.1)
 *   - Boot button on GPIO 0
 *
 * Note: The onboard LED is a WS2812 addressable RGB LED which
 * requires a NeoPixel library — simple digitalWrite won't work.
 * LED feedback is disabled for this board (LED_PIN = -1).
 *
 * Purchase: https://www.espressif.com/en/products/devkits
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
#define BOARD_NAME "ESP32-S3-DevKitC-1"

// ─── Pin assignments ────────────────────────────────────────────────────────
// RGB LED on GPIO 48 (WS2812, not usable with digitalWrite)
#define BOARD_LED_PIN -1
#define BOARD_BOOT_PIN 0

// ─── LED behavior ───────────────────────────────────────────────────────────
#define BOARD_LED_INVERTED false
