/**
 * Board Configuration Template — Daydream Air Mouse
 *
 * Copy this file and rename it to match your board, e.g.:
 *   boards/esp32s3_devkit.h
 *
 * Then update the values below to match your board's hardware.
 *
 * ── Board Requirements ──────────────────────────────────────────────────────
 *
 *   ✅ REQUIRED:
 *     - ESP32-S3 chip (BLE 5.0 + native USB-OTG)
 *     - At least 4MB flash
 *     - Native USB port (NOT a USB-UART bridge like CP2102/CH340)
 *     - Arduino framework support in PlatformIO
 *
 *   ⚠️ NICE TO HAVE:
 *     - Built-in LED (for status feedback)
 *     - Boot/reset button (for controller switching)
 *     - Small form factor (to hide in a USB adapter)
 *
 *   ❌ WON'T WORK:
 *     - ESP32 (original) — no native USB-OTG
 *     - ESP32-C3 — no USB HID support
 *     - Boards with USB-UART bridge only (no native USB)
 *
 * ── After Creating Your Config ──────────────────────────────────────────────
 *
 *   1. Add a PlatformIO environment in platformio.ini:
 *
 *        [env:your_board_name]
 *        extends = common
 *        board = your_platformio_board_id
 *        build_flags =
 *            ${common.build_flags}
 *            -DBOARD_YOUR_BOARD_NAME
 *
 *   2. Add the board selection to main.cpp (see the #ifdef chain at the top)
 *
 *   3. Build with: platformio run -e your_board_name
 *
 *   4. To contribute: PR your board config + pre-built firmware to the repo!
 *      See CONTRIBUTING.md for details.
 */

#pragma once

// ─── Board identity ─────────────────────────────────────────────────────────
// Human-readable name shown in serial output
#define BOARD_NAME "My Custom Board"

// ─── Pin assignments ────────────────────────────────────────────────────────
// LED pin for status feedback. Use LED_BUILTIN if your board defines it,
// or specify a GPIO number directly (e.g., 2, 48, etc.)
// Set to -1 if your board has no usable LED.
#define BOARD_LED_PIN LED_BUILTIN

// Boot/BOOT button pin. Most ESP32-S3 boards have this on GPIO 0.
// Set to -1 if your board has no button.
#define BOARD_BOOT_PIN 0

// ─── LED behavior ───────────────────────────────────────────────────────────
// true  = LED turns ON when pin is LOW  (common for built-in LEDs)
// false = LED turns ON when pin is HIGH (common for external LEDs)
#define BOARD_LED_INVERTED true
