/**
 * Board Configuration — Seeed XIAO ESP32-S3
 *
 * Official reference board for Daydream Air Mouse.
 * This is the default board config.
 *
 * Specs:
 *   - ESP32-S3 (BLE 5.0 + native USB OTG)
 *   - 8MB Flash, 8MB PSRAM
 *   - 21×17.5mm form factor
 *   - Built-in LED on GPIO 21 (active LOW)
 *   - Boot button on GPIO 0
 *
 * Purchase: https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
#define BOARD_NAME "Seeed XIAO ESP32-S3"

// ─── Pin assignments ────────────────────────────────────────────────────────
#define BOARD_LED_PIN LED_BUILTIN // GPIO 21
#define BOARD_BOOT_PIN 0          // Boot/reset button

// ─── LED behavior ───────────────────────────────────────────────────────────
// Set to true if the LED turns ON when the pin is pulled LOW (common for
// built-in LEDs on many ESP32 boards). Set to false if HIGH = ON.
#define BOARD_LED_INVERTED true

// ─── USB ────────────────────────────────────────────────────────────────────
// The XIAO ESP32-S3 uses the native USB-OTG peripheral for HID.
// Most ESP32-S3 boards support this. If your board uses a USB-UART bridge
// chip (CP2102, CH340) instead, USB HID will NOT work.
