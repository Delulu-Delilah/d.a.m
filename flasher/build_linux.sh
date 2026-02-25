#!/bin/bash
# Build Linux executable for the Daydream Air Mouse Flasher
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Activate venv if it exists
if [ -d "venv" ]; then
    source venv/bin/activate
fi

echo "Building Daydream Air Mouse Flasher for Linux..."

pyinstaller \
    --name "daydream-air-mouse-flasher" \
    --onedir \
    --windowed \
    --noconfirm \
    --clean \
    --add-data "firmware:firmware" \
    --hidden-import customtkinter \
    --hidden-import esptool \
    --hidden-import serial \
    --hidden-import serial.tools \
    --hidden-import serial.tools.list_ports \
    --collect-all customtkinter \
    --collect-all esptool \
    flasher.py

echo ""
echo "Build complete!"
echo "Executable: dist/daydream-air-mouse-flasher/daydream-air-mouse-flasher"
echo ""
echo "To create an AppImage, install appimagetool and run:"
echo "  appimagetool dist/daydream-air-mouse-flasher/"
