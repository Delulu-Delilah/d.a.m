# Contributing to Daydream Air Mouse

Want to add support for your ESP32 board? Great — it's designed to be easy.

## Board Requirements

Your board **must** have:

| Requirement | Why |
|-------------|-----|
| **ESP32-S3 chip** | Only the S3 has both BLE 5.0 and native USB-OTG |
| **Native USB port** | Required for USB HID (mouse/keyboard) — boards with only a USB-UART bridge (CP2102, CH340) won't work |
| **4MB+ flash** | Firmware is ~600KB, plus NVS storage |
| **Arduino framework support** | Must be supported by PlatformIO's Arduino framework |

Nice to have:

- **Built-in LED** → status feedback (breathing/solid/off)
- **Boot/BOOT button** → controller slot switching
- **Small form factor** → fits inside USB adapter cases

### Compatible Chips

| Chip | BLE | USB HID | Status |
|------|-----|---------|--------|
| ESP32-S3 | Yes | Yes (Native USB-OTG) | **Supported** |
| ESP32-S2 | No BLE | Yes (Native USB-OTG) | Won't work |
| ESP32 (original) | Yes | No native USB | Won't work |
| ESP32-C3 | Yes | No USB HID | Won't work |
| ESP32-C6 | Yes | No USB HID | Won't work |
| ESP32-H2 | Yes | No native USB | Won't work |

> **TL;DR**: You need an **ESP32-S3** board with a **native USB** port.

## Adding a New Board

### 1. Create the board config

Copy the template:

```bash
cp boards/board_template.h boards/your_board.h
```

Edit it with your board's pin assignments:

```c
#define BOARD_NAME            "Your Board Name"
#define BOARD_LED_PIN         2             // or LED_BUILTIN
#define BOARD_BOOT_PIN        0             // or -1 if no button
#define BOARD_LED_INVERTED    false         // true if LED is active LOW
```

### 2. Add the board to main.cpp

Find the board selection block near the top of `src/main.cpp`:

```c
#if defined(BOARD_XIAO_ESP32S3)
  #include "../boards/xiao_esp32s3.h"
// ── Add new boards here ──
#elif defined(BOARD_YOUR_BOARD)
  #include "../boards/your_board.h"
#else
  #include "../boards/xiao_esp32s3.h"
#endif
```

### 3. Add a PlatformIO environment

In `platformio.ini`, add a new environment:

```ini
[env:your_board]
extends = common
platform = espressif32
board = your_platformio_board_id
build_flags =
    ${common.build_flags}
    -DBOARD_YOUR_BOARD
```

### 4. Build and test

```bash
platformio run -e your_board
```

### 5. Submit a PR

Include:
- [x] `boards/your_board.h` — board config
- [x] Updated `src/main.cpp` — `#elif` for your board
- [x] Updated `platformio.ini` — new `[env:xxx]` section
- [x] Pre-built firmware in `docs/boards/your_board/firmware/` (bootloader.bin, partitions.bin, firmware.bin)
- [x] Updated `docs/boards/your_board/manifest.json`

### Building firmware binaries for the web flasher

After a successful build, copy the binary files:

```bash
mkdir -p docs/boards/your_board/firmware
cp .pio/build/your_board/bootloader.bin docs/boards/your_board/firmware/
cp .pio/build/your_board/partitions.bin docs/boards/your_board/firmware/
cp .pio/build/your_board/firmware.bin   docs/boards/your_board/firmware/
```

Create `docs/boards/your_board/manifest.json`:

```json
{
  "name": "Daydream Air Mouse — Your Board",
  "version": "1.0",
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "firmware/bootloader.bin", "offset": 0 },
        { "path": "firmware/partitions.bin", "offset": 32768 },
        { "path": "firmware/firmware.bin",   "offset": 65536 }
      ]
    }
  ]
}
```

Your board will appear in the web flasher's board selector once your PR is merged!

## Code Structure

```
boards/
  xiao_esp32s3.h          ← Reference board config
  board_template.h        ← Copy this for new boards
  your_board.h            ← Your board config

src/
  main.cpp                ← Firmware (board-agnostic)

docs/boards/
  xiao_esp32s3/           ← Official firmware
    manifest.json
    firmware/
  your_board/             ← Community firmware
    manifest.json
    firmware/
```

## Questions?

Open an issue on [GitHub](https://github.com/Delulu-Delilah/daydream-airmouse/issues).
