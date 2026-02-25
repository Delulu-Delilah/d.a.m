#!/usr/bin/env python3
"""
Daydream Air Mouse — Firmware Flasher
Cross-platform GUI tool for flashing the Daydream Air Mouse firmware
onto a Seeed XIAO ESP32-S3 board.

Features:
  - Auto-detects ESP32 serial ports
  - Visual progress bar + real-time log
  - Post-flash verification (reads serial to confirm boot)
  - Friendly error messages with recovery tips
"""

import sys
import os
import threading
import io
import re
import time
import customtkinter as ctk
from serial.tools import list_ports

# ─── Firmware path resolution ────────────────────────────────────────────────

def get_firmware_dir():
    """Get path to firmware directory, works both in dev and PyInstaller bundle."""
    if getattr(sys, '_MEIPASS', None):
        return os.path.join(sys._MEIPASS, 'firmware')
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), 'firmware')


def check_firmware_files():
    """Verify all required firmware files exist."""
    fw_dir = get_firmware_dir()
    files = {
        'bootloader': os.path.join(fw_dir, 'bootloader.bin'),
        'partitions': os.path.join(fw_dir, 'partitions.bin'),
        'firmware': os.path.join(fw_dir, 'firmware.bin'),
    }
    missing = [name for name, path in files.items() if not os.path.isfile(path)]
    return files, missing


# ─── Flash addresses for ESP32-S3 ────────────────────────────────────────────

FLASH_ADDRESSES = {
    'bootloader': 0x0,
    'partitions': 0x8000,
    'firmware':   0x10000,
}

# ─── Serial port helpers ─────────────────────────────────────────────────────

# Keywords that suggest an ESP32 device
ESP32_KEYWORDS = [
    'USB JTAG', 'USB Serial', 'CP210', 'CH340', 'CH910',
    'FTDI', 'Silicon Labs', 'ESP32', 'usbmodem', 'usbserial',
    'ttyUSB', 'ttyACM', 'wchusbserial', 'SLAB_USB',
]

# Keywords for devices to filter OUT
FILTER_KEYWORDS = [
    'Bluetooth', 'EDIFIER', 'AIWA', 'MOCUTE', 'debug-console',
    'AirPods', 'Beats', 'JBL',
]


def get_serial_ports():
    """Return list of (port, description, is_esp32) tuples."""
    ports = list_ports.comports()
    result = []
    for p in ports:
        combined = f"{p.device} {p.description} {p.manufacturer or ''}"

        # Filter out non-serial devices
        if any(kw.lower() in combined.lower() for kw in FILTER_KEYWORDS):
            continue

        # Check if this looks like an ESP32
        is_esp32 = any(kw.lower() in combined.lower() for kw in ESP32_KEYWORDS)

        label = f"{p.device}"
        if p.description and p.description != 'n/a':
            label += f"  ({p.description})"
        if is_esp32:
            label = f"⚡ {label}"

        result.append((p.device, label, is_esp32))

    # Sort ESP32 devices first
    result.sort(key=lambda x: (not x[2], x[0]))
    return result


def verify_flash(port, timeout=5):
    """After flashing, briefly read serial to confirm firmware booted."""
    try:
        import serial
        ser = serial.Serial(port, 115200, timeout=1)
        start = time.time()
        output = ""
        while time.time() - start < timeout:
            data = ser.read(256)
            if data:
                output += data.decode('utf-8', errors='replace')
                if 'Daydream Air Mouse' in output or 'BLE' in output or 'USB' in output:
                    ser.close()
                    return True, output
        ser.close()
        return False, output
    except Exception as e:
        return False, str(e)


# ─── Error messages ──────────────────────────────────────────────────────────

ERROR_TIPS = {
    'could not open port': (
        "Port is busy or not found.\n\n"
        "💡 Tips:\n"
        "  • Unplug and replug the USB cable\n"
        "  • Close any serial monitors (Arduino IDE, PlatformIO, etc.)\n"
        "  • Try a different USB port"
    ),
    'failed to connect': (
        "Couldn't connect to the ESP32.\n\n"
        "💡 Tips:\n"
        "  • Hold the BOOT button while plugging in the USB cable\n"
        "  • Try a different USB cable (some are charge-only)\n"
        "  • Make sure you selected the correct port"
    ),
    'wrong chip': (
        "The detected chip doesn't match ESP32-S3.\n\n"
        "💡 Make sure you're using a Seeed XIAO ESP32-S3 board."
    ),
    'permission': (
        "No permission to access the serial port.\n\n"
        "💡 Tips:\n"
        "  • On Linux: add your user to the 'dialout' group\n"
        "    sudo usermod -a -G dialout $USER\n"
        "  • On macOS: the port might be in use by another app"
    ),
}


def get_friendly_error(error_text):
    """Match error text to a friendly tip message."""
    lower = error_text.lower()
    for key, tip in ERROR_TIPS.items():
        if key in lower:
            return tip
    return None


# ─── CustomTkinter App ───────────────────────────────────────────────────────

class FlasherApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        # ── Window setup ──
        self.title("Daydream Air Mouse — Flasher")
        self.geometry("700x680")
        self.minsize(600, 560)
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")

        self.flashing = False
        self.selected_port = ctk.StringVar(value="")
        self.port_list = []

        self._build_ui()
        self._refresh_ports()
        self._check_firmware()

    def _build_ui(self):
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(5, weight=1)

        pad = {"padx": 24}

        # ── Header ──
        header = ctk.CTkFrame(self, fg_color="transparent")
        header.grid(row=0, column=0, sticky="ew", **pad, pady=(24, 0))
        header.grid_columnconfigure(0, weight=1)

        title = ctk.CTkLabel(
            header, text="🎮  Daydream Air Mouse",
            font=ctk.CTkFont(size=28, weight="bold"),
        )
        title.grid(row=0, column=0, sticky="w")

        subtitle = ctk.CTkLabel(
            header, text="Flash firmware to your ESP32-S3 board",
            font=ctk.CTkFont(size=14),
            text_color=("gray50", "gray60"),
        )
        subtitle.grid(row=1, column=0, sticky="w", pady=(2, 0))

        version = ctk.CTkLabel(
            header, text="v1.1",
            font=ctk.CTkFont(size=12),
            text_color=("gray40", "gray50"),
        )
        version.grid(row=0, column=1, sticky="ne", padx=(8, 0))

        # ── Steps card ──
        info = ctk.CTkFrame(self, corner_radius=12)
        info.grid(row=1, column=0, sticky="ew", padx=24, pady=(16, 0))
        info.grid_columnconfigure(0, weight=1)

        steps = ctk.CTkLabel(
            info,
            text=(
                "1.  Connect your XIAO ESP32-S3 via USB\n"
                "2.  Select the serial port below  (⚡ = auto-detected ESP32)\n"
                "3.  Click Flash Firmware"
            ),
            font=ctk.CTkFont(size=13), justify="left", anchor="w",
        )
        steps.grid(row=0, column=0, sticky="w", padx=16, pady=12)

        # ── Port selector ──
        port_frame = ctk.CTkFrame(self, fg_color="transparent")
        port_frame.grid(row=2, column=0, sticky="ew", padx=24, pady=(16, 0))
        port_frame.grid_columnconfigure(1, weight=1)

        port_label = ctk.CTkLabel(
            port_frame, text="Serial Port",
            font=ctk.CTkFont(size=13, weight="bold"),
        )
        port_label.grid(row=0, column=0, sticky="w", padx=(0, 12))

        self.port_menu = ctk.CTkOptionMenu(
            port_frame,
            variable=self.selected_port,
            values=["Scanning..."],
            width=380, height=36,
            font=ctk.CTkFont(size=13),
            command=self._on_port_selected,
        )
        self.port_menu.grid(row=0, column=1, sticky="ew")

        self.refresh_btn = ctk.CTkButton(
            port_frame, text="↻", width=36, height=36,
            font=ctk.CTkFont(size=18),
            command=self._refresh_ports,
        )
        self.refresh_btn.grid(row=0, column=2, padx=(8, 0))

        # ── Flash button + progress bar ──
        action_frame = ctk.CTkFrame(self, fg_color="transparent")
        action_frame.grid(row=3, column=0, sticky="ew", padx=24, pady=(16, 0))
        action_frame.grid_columnconfigure(0, weight=1)

        self.flash_btn = ctk.CTkButton(
            action_frame,
            text="⚡  Flash Firmware",
            font=ctk.CTkFont(size=16, weight="bold"),
            height=48, corner_radius=12,
            fg_color=("#2563eb", "#1d4ed8"),
            hover_color=("#1e40af", "#1e3a8a"),
            command=self._start_flash,
            state="disabled",
        )
        self.flash_btn.grid(row=0, column=0, sticky="ew")

        # Progress bar
        self.progress_bar = ctk.CTkProgressBar(
            action_frame, height=6, corner_radius=3,
            progress_color=("#3b82f6", "#2563eb"),
        )
        self.progress_bar.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        self.progress_bar.set(0)

        self.progress_label = ctk.CTkLabel(
            action_frame, text="",
            font=ctk.CTkFont(size=11),
            text_color=("gray50", "gray60"), anchor="w",
        )
        self.progress_label.grid(row=2, column=0, sticky="w", pady=(2, 0))

        # ── Log output ──
        log_frame = ctk.CTkFrame(self, corner_radius=12)
        log_frame.grid(row=5, column=0, sticky="nsew", padx=24, pady=(12, 0))
        log_frame.grid_columnconfigure(0, weight=1)
        log_frame.grid_rowconfigure(0, weight=1)

        self.log_text = ctk.CTkTextbox(
            log_frame,
            font=ctk.CTkFont(family="Menlo, Consolas, monospace", size=12),
            corner_radius=8, wrap="word", state="disabled",
        )
        self.log_text.grid(row=0, column=0, sticky="nsew", padx=8, pady=8)

        # ── Status bar ──
        self.status_label = ctk.CTkLabel(
            self, text="Ready",
            font=ctk.CTkFont(size=12),
            text_color=("gray50", "gray60"), anchor="w",
        )
        self.status_label.grid(row=6, column=0, sticky="ew", padx=28, pady=(8, 16))

    # ── Port management ──

    def _refresh_ports(self):
        ports = get_serial_ports()
        if ports:
            self.port_list = ports
            labels = [label for _, label, _ in ports]
            self.port_menu.configure(values=labels)

            # Auto-select first ESP32 device, or first device
            esp_ports = [l for _, l, is_esp in ports if is_esp]
            if esp_ports:
                self.selected_port.set(esp_ports[0])
                self._log(f"Auto-detected ESP32: {esp_ports[0]}")
            elif not self.selected_port.get() or self.selected_port.get() == "No ports detected":
                self.selected_port.set(labels[0])

            self.flash_btn.configure(state="normal")
        else:
            self.port_list = []
            self.port_menu.configure(values=["No ports detected"])
            self.selected_port.set("No ports detected")
            self.flash_btn.configure(state="disabled")

    def _on_port_selected(self, choice):
        if choice != "No ports detected":
            self.flash_btn.configure(state="normal")

    def _get_selected_device(self):
        label = self.selected_port.get()
        for device, plabel, _ in self.port_list:
            if plabel == label:
                return device
        return None

    # ── Firmware check ──

    def _check_firmware(self):
        files, missing = check_firmware_files()
        if missing:
            self._log(f"⚠ Missing firmware files: {', '.join(missing)}")
            self._set_status("Error: Missing firmware files", error=True)
            self.flash_btn.configure(state="disabled")
        else:
            sizes = {name: os.path.getsize(path) for name, path in files.items()}
            self._log("Firmware files ready:")
            for name, size in sizes.items():
                self._log(f"  {name}.bin — {size:,} bytes → 0x{FLASH_ADDRESSES[name]:05X}")
            self._log("")

    # ── Logging ──

    def _log(self, text):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    def _set_status(self, text, error=False, success=False):
        color = ("gray50", "gray60")
        if error:
            color = ("#ef4444", "#f87171")
        elif success:
            color = ("#22c55e", "#4ade80")
        self.status_label.configure(text=text, text_color=color)

    def _set_progress(self, value, label=""):
        self.progress_bar.set(value)
        self.progress_label.configure(text=label)

    # ── Flashing ──

    def _start_flash(self):
        if self.flashing:
            return

        port = self._get_selected_device()
        if not port:
            self._log("⚠ No serial port selected!")
            return

        files, missing = check_firmware_files()
        if missing:
            self._log(f"⚠ Missing firmware: {', '.join(missing)}")
            return

        self.flashing = True
        self.flash_btn.configure(state="disabled", text="⏳  Flashing...")
        self.refresh_btn.configure(state="disabled")
        self.port_menu.configure(state="disabled")
        self._set_status("Connecting to ESP32...")
        self._set_progress(0, "Connecting...")

        self._log("─" * 50)
        self._log(f"Flashing to {port}...")
        self._log("")

        thread = threading.Thread(
            target=self._flash_thread,
            args=(port, files),
            daemon=True,
        )
        thread.start()

    def _flash_thread(self, port, files):
        try:
            import esptool

            args = [
                '--chip', 'esp32s3',
                '--port', port,
                '--baud', '460800',
                '--before', 'default_reset',
                '--after', 'hard_reset',
                'write_flash',
                '-z',
                '--flash_mode', 'dio',
                '--flash_freq', '80m',
                '--flash_size', '8MB',
                hex(FLASH_ADDRESSES['bootloader']), files['bootloader'],
                hex(FLASH_ADDRESSES['partitions']), files['partitions'],
                hex(FLASH_ADDRESSES['firmware']), files['firmware'],
            ]

            self.after(0, self._set_progress, 0.05, "Erasing flash...")
            self.after(0, self._set_status, "Erasing flash...")

            # Capture output
            old_stdout = sys.stdout
            old_stderr = sys.stderr
            capture = io.StringIO()
            sys.stdout = capture
            sys.stderr = capture

            try:
                esptool.main(args)
                success = True
            except SystemExit as e:
                success = (e.code == 0 or e.code is None)
            except Exception as e:
                self.after(0, self._log, f"Error: {e}")
                success = False
            finally:
                sys.stdout = old_stdout
                sys.stderr = old_stderr

            # Parse output for progress
            output = capture.getvalue()
            lines = output.splitlines()
            for line in lines:
                line = line.strip()
                if not line:
                    continue

                # Parse writing progress percentages
                match = re.search(r'\((\d+)\s*%\)', line)
                if match:
                    pct = int(match.group(1))
                    progress = 0.1 + (pct / 100.0) * 0.8  # 10-90%
                    self.after(0, self._set_progress, progress,
                              f"Writing... {pct}%")
                    self.after(0, self._set_status, f"Writing firmware... {pct}%")

                self.after(0, self._log, line)

            if success:
                self.after(0, self._set_progress, 0.95, "Verifying...")
                self.after(0, self._set_status, "Verifying firmware boot...")
                self.after(0, self._log, "")
                self.after(0, self._log, "Verifying firmware boot...")

                # Brief delay for reset
                time.sleep(2)

                # Try to read serial to verify firmware booted
                verified, verify_output = verify_flash(port, timeout=4)

                if verified:
                    self.after(0, self._log, "✅  Firmware verified! Board is running.")
                    self.after(0, self._set_progress, 1.0, "Complete!")
                    self.after(0, self._set_status,
                               "✅ Flash complete! Turn on your Daydream controller to pair.",
                               False, True)
                else:
                    self.after(0, self._log, "✅  Firmware flashed! (Could not verify boot — this is normal)")
                    self.after(0, self._set_progress, 1.0, "Complete!")
                    self.after(0, self._set_status,
                               "Flash complete! Unplug and replug to start.",
                               False, True)
            else:
                self.after(0, self._set_progress, 0, "Failed")
                self.after(0, self._log, "")

                # Check for friendly error
                tip = get_friendly_error(output)
                if tip:
                    self.after(0, self._log, "─" * 50)
                    self.after(0, self._log, tip)
                else:
                    self.after(0, self._log, "❌  Flash failed! Check the log above.")

                self.after(0, self._set_status, "Flash failed — see log for tips", True, False)

        except ImportError:
            self.after(0, self._log, "❌ esptool not installed! Run: pip install esptool")
            self.after(0, self._set_status, "Error: esptool not found", True, False)
        except Exception as e:
            error_str = str(e)
            self.after(0, self._log, f"❌ Error: {error_str}")

            tip = get_friendly_error(error_str)
            if tip:
                self.after(0, self._log, "")
                self.after(0, self._log, tip)

            self.after(0, self._set_status, f"Error: {error_str}", True, False)
        finally:
            self.after(0, self._flash_complete)

    def _flash_complete(self):
        self.flashing = False
        self.flash_btn.configure(state="normal", text="⚡  Flash Firmware")
        self.refresh_btn.configure(state="normal")
        self.port_menu.configure(state="normal")


# ─── Main ────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    app = FlasherApp()
    app.mainloop()
