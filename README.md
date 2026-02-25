# 🎮 Daydream Air Mouse

Turn a Google Daydream controller into a wireless USB air mouse and media remote — no phone, no headset, no Google apps needed.

Uses a **Seeed XIAO ESP32-S3** as a tiny BLE-to-USB bridge that connects to the Daydream controller over Bluetooth and presents itself as a standard USB HID mouse + media controller to any computer.

## Features

- **Three modes** — cycle with Home button:
  - ✈️ **Air Mouse** — point and move, orientation-based cursor control with EMA smoothing
  - 🖱️ **Trackpad** — use the Daydream touchpad as a mouse trackpad
  - 🎵 **Media** — swipe for next/previous track, click for play/pause, volume buttons
- **Dual controller support** — connect two Daydream controllers simultaneously, switch with App + Volume Down combo
- **Updated firmware compatible** — works with both old and new Daydream controller firmware (BLE bonding + CCCD fix)
- **Cross-platform flasher app** — one-click GUI tool to flash firmware, no developer tools needed

## Hardware

| Part | Purpose |
|------|---------|
| [Seeed XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) | BLE → USB bridge (21×17.5mm) |
| Google Daydream Controller | Wireless input device |
| USB-C cable | Power + data to computer |

The XIAO ESP32-S3 is one of the smallest ESP32-S3 boards available — small enough to hide inside a USB adapter case.

## Quick Start

### Option 1: Use the Flasher App (recommended)

1. Download the flasher app for your OS from [Releases](../../releases)
2. Plug in your XIAO ESP32-S3 via USB
3. Open the app, select the serial port, click **Flash Firmware**
4. Done! Hold the Home button on your Daydream controller to pair

### Option 2: Build from Source

Requires [PlatformIO](https://platformio.org/):

```bash
# Clone the repo
git clone https://github.com/Delulu-Delilah/daydream-airmouse.git
cd daydream-airmouse

# Build and flash
platformio run --target upload
```

## Controls

### All Modes
| Input | Action |
|-------|--------|
| Home (short press) | Cycle mode: Air Mouse → Trackpad → Media |
| Home (hold 1s) | Recenter orientation (air mouse) |
| App + Vol Down | Switch active controller (dual mode) |

### Air Mouse & Trackpad Mode
| Input | Action |
|-------|--------|
| Trackpad click | Left click |
| App button | Middle click |
| Vol Down | Right click |
| Trackpad swipe (air mouse) | Scroll wheel |

### Media Mode
| Input | Action |
|-------|--------|
| Trackpad click | Play / Pause |
| Swipe right | Next track |
| Swipe left | Previous track |
| App button | Mute |
| Vol Up / Vol Down | Volume up / down |

## Building the Flasher App

The flasher is built with Python + CustomTkinter + esptool:

```bash
cd flasher
python3 -m venv venv
source venv/bin/activate  # or venv\Scripts\activate on Windows
pip install -r requirements.txt

# macOS
bash build_mac.sh

# Windows
build_win.bat

# Linux
bash build_linux.sh
```

## Project Structure

```
daydream-airmouse/
├── src/
│   └── main.cpp              # ESP32-S3 firmware
├── platformio.ini             # PlatformIO config
├── flasher/
│   ├── flasher.py             # Cross-platform flasher GUI
│   ├── firmware/              # Pre-built firmware binaries
│   │   ├── bootloader.bin
│   │   ├── partitions.bin
│   │   └── firmware.bin
│   ├── requirements.txt
│   ├── build_mac.sh
│   ├── build_win.bat
│   └── build_linux.sh
└── README.md
```

## Credits

- Inspired by [Daydream2HID](https://github.com/ryukoposting/daydream2hid) (Zephyr/nRF52840 approach)
- Daydream BLE protocol based on community reverse-engineering efforts
- Built with [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino), [CustomTkinter](https://github.com/TomSchimansky/CustomTkinter), and [esptool](https://github.com/espressif/esptool)

## License

MIT
