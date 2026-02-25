# 🎮 Daydream Air Mouse

Turn a Google Daydream controller into a wireless USB air mouse and media remote — no phone, no headset, no Google apps needed.

Uses a **Seeed XIAO ESP32-S3** as a tiny BLE-to-USB bridge that connects to the Daydream controller over Bluetooth and presents itself as a standard USB HID mouse + media controller to any computer.

## Features

- **Three modes** — cycle with Home button:
  - ✈️ **Air Mouse** — point and move, orientation-based cursor control with EMA smoothing
  - 🖱️ **Trackpad** — use the Daydream touchpad as a mouse trackpad
  - 🎵 **Media** — swipe for next/previous track, click for play/pause, volume buttons
- **Dual controller support** — connect two Daydream controllers simultaneously, switch with App + Volume Down combo
- **Adjustable sensitivity** — Home + Vol Up/Down to adjust on the fly, auto-saved to flash
- **Smart LED feedback** — breathing while scanning, solid when connected, off when sleeping
- **Auto-sleep** — stops scanning after 2 min if no controller found, press Boot to wake
- **Updated firmware compatible** — works with both old and new Daydream controller firmware (BLE bonding + CCCD fix)

## Hardware

| Part | Purpose |
|------|---------|
| [Seeed XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) | BLE → USB bridge (21×17.5mm) |
| Google Daydream Controller | Wireless input device |
| USB-C cable | Power + data to computer |

The XIAO ESP32-S3 is one of the smallest ESP32-S3 boards available — small enough to hide inside a USB adapter case.

## Quick Start

### Option 1: Web Installer (recommended)

Flash directly from your browser — no downloads needed:

### **👉 [Install Firmware](https://delulu-delilah.github.io/daydream-airmouse/)**

> Requires **Chrome** or **Edge** on desktop. Connect your XIAO ESP32-S3 via USB, click Install, select the port, done.

### Option 2: Build from Source

Requires [PlatformIO](https://platformio.org/):

```bash
git clone https://github.com/Delulu-Delilah/daydream-airmouse.git
cd daydream-airmouse
platformio run --target upload
```

## Controls

### All Modes
| Input | Action |
|-------|--------|
| Home (short press) | Cycle mode: Air Mouse → Trackpad → Media |
| Home (hold 1s) | Recenter orientation (air mouse) |
| App + Vol Down | Switch active controller (dual mode) |
| Home + Vol Up | Increase sensitivity (saved to flash) |
| Home + Vol Down | Decrease sensitivity (saved to flash) |

### Air Mouse & Trackpad Mode
| Input | Action |
|-------|--------|
| Trackpad click | Left click |
| App button | Right click |
| Vol Up / Vol Down | Scroll up / down |
| Trackpad swipe (air mouse) | Scroll wheel |

### Media Mode
| Input | Action |
|-------|--------|
| Trackpad click | Play / Pause |
| Swipe right | Next track |
| Swipe left | Previous track |
| App button | Mute |
| Vol Up / Vol Down | Volume up / down |

### LED States
| Pattern | Meaning |
|---------|---------|
| 💨 Breathing | Scanning for controllers |
| 🔵 Solid | Connected |
| ⚫ Off | Sleeping (press Boot to wake) |
| 1 flash | Slot 0 active |
| 2 flashes | Slot 1 active |

## Project Structure

```
daydream-airmouse/
├── src/
│   └── main.cpp          # ESP32-S3 firmware
├── platformio.ini         # PlatformIO config
├── docs/                  # Web flasher (GitHub Pages)
│   ├── index.html         # ESP Web Tools install page
│   ├── manifest.json      # Firmware manifest
│   └── firmware/          # Pre-built binaries
└── README.md
```

## Credits

- Inspired by [Daydream2HID](https://github.com/ryukoposting/daydream2hid) (Zephyr/nRF52840 approach)
- Daydream BLE protocol based on community reverse-engineering efforts
- Built with [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) and [ESP Web Tools](https://esphome.github.io/esp-web-tools/)

## License

MIT
