@echo off
REM Build Windows .exe for the Daydream Air Mouse Flasher

echo Building Daydream Air Mouse Flasher for Windows...

pyinstaller ^
    --name "Daydream Air Mouse Flasher" ^
    --onedir ^
    --windowed ^
    --noconfirm ^
    --clean ^
    --add-data "firmware;firmware" ^
    --hidden-import customtkinter ^
    --hidden-import esptool ^
    --hidden-import serial ^
    --hidden-import serial.tools ^
    --hidden-import serial.tools.list_ports ^
    --collect-all customtkinter ^
    --collect-all esptool ^
    flasher.py

echo.
echo Build complete!
echo Executable: dist\Daydream Air Mouse Flasher\Daydream Air Mouse Flasher.exe
