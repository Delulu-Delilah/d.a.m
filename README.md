# Daydream Air Mouse

Turn a Google Daydream controller into a wireless USB air mouse and media remote — no phone, no headset, no Google apps needed.

Uses an **ESP32-S3** board as a tiny BLE-to-USB bridge that connects to the Daydream controller over Bluetooth and presents itself as a standard USB HID mouse + media controller to any computer.

## Features

- **Three modes** — cycle with Home button:
  - **Air Mouse** — point and move, orientation-based cursor control with EMA smoothing
  - **Trackpad** — use the Daydream touchpad as a mouse trackpad
  - **Media** — swipe for next/previous track, click for play/pause, volume buttons
- **Dual controller support** — connect two Daydream controllers simultaneously
- **Adjustable sensitivity** — Home + Vol Up/Down, auto-saved to flash
- **Smart LED feedback** — breathing while scanning, solid when connected, off when sleeping
- **Auto-sleep** — stops scanning after 2 min, Boot button to wake
- **Multi-board support** — bring your own ESP32-S3 board ([add yours!](CONTRIBUTING.md))

## Supported Boards

| Board | Status | Notes |
|-------|--------|-------|
| [Seeed XIAO ESP32-S3](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) | Official | 21×17.5mm, smallest option |

> **Want to add your board?** See [CONTRIBUTING.md](CONTRIBUTING.md) — any ESP32-S3 board with native USB works.

## Quick Start

### Option 1: Web Installer (recommended)

Flash directly from your browser — no downloads needed:

### **[Install Firmware](https://delulu-delilah.github.io/daydream-airmouse/)**

> Requires **Chrome** or **Edge** on desktop. Select your board, connect via USB, click Install.

### Option 2: Build from Source

Requires [PlatformIO](https://platformio.org/):

```bash
git clone https://github.com/Delulu-Delilah/daydream-airmouse.git
cd daydream-airmouse

# Build for XIAO ESP32-S3 (default)
platformio run -e xiao_esp32s3

# Flash
platformio run -e xiao_esp32s3 --target upload
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
| Breathing | Scanning for controllers |
| Solid | Connected |
| Off | Sleeping (press Boot to wake) |

## Project Structure

```
daydream-airmouse/
├── src/main.cpp               # Firmware (board-agnostic)
├── boards/                    # Board configs
│   ├── xiao_esp32s3.h         # Official board
│   └── board_template.h       # Template for new boards
├── platformio.ini             # Multi-board PlatformIO config
├── docs/                      # Web flasher (GitHub Pages)
│   ├── index.html             # Install page with board selector
│   └── boards/                # Per-board firmware + manifests
├── CONTRIBUTING.md            # How to add your board
└── README.md
```

## Credits

- Inspired by [Daydream2HID](https://github.com/ryukoposting/daydream2hid)
- Built with [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) and [ESP Web Tools](https://esphome.github.io/esp-web-tools/)

## License

MIT
