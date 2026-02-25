#!/usr/bin/env python3
"""
Daydream Air Mouse — Firmware Flasher
Cross-platform GUI tool for flashing the Daydream Air Mouse firmware
onto a Seeed XIAO ESP32-S3 board.

Uses CustomTkinter for the UI and esptool for flashing.
"""

import sys
import os
import threading
import io
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

def get_serial_ports():
    """Return list of (port, description) tuples for available serial ports."""
    ports = list_ports.comports()
    result = []
    for p in ports:
        # Filter out Bluetooth ports on macOS
        if 'Bluetooth' in p.description or 'Bluetooth' in p.device:
            continue
        if 'EDIFIER' in p.device or 'AIWA' in p.device or 'MOCUTE' in p.device:
            continue
        label = f"{p.device}"
        if p.description and p.description != 'n/a':
            label += f"  ({p.description})"
        result.append((p.device, label))
    return result


# ─── CustomTkinter App ───────────────────────────────────────────────────────

class FlasherApp(ctk.CTk):
    def __init__(self):
        super().__init__()

        # ── Window setup ──
        self.title("Daydream Air Mouse — Flasher")
        self.geometry("680x620")
        self.minsize(580, 520)
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("blue")

        self.flashing = False
        self.selected_port = ctk.StringVar(value="")

        self._build_ui()
        self._refresh_ports()
        self._check_firmware()

    def _build_ui(self):
        # ── Main container ──
        self.grid_columnconfigure(0, weight=1)
        self.grid_rowconfigure(4, weight=1)

        pad = {"padx": 24, "pady": (0, 0)}

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

        # ── Info card ──
        info = ctk.CTkFrame(self, corner_radius=12)
        info.grid(row=1, column=0, sticky="ew", padx=24, pady=(16, 0))
        info.grid_columnconfigure(0, weight=1)

        steps_text = (
            "1.  Connect your XIAO ESP32-S3 via USB\n"
            "2.  Select the serial port below\n"
            "3.  Click Flash Firmware"
        )
        steps_label = ctk.CTkLabel(
            info, text=steps_text,
            font=ctk.CTkFont(size=13),
            justify="left", anchor="w",
        )
        steps_label.grid(row=0, column=0, sticky="w", padx=16, pady=12)

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
            values=["No ports detected"],
            width=360,
            height=36,
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

        # ── Flash button ──
        self.flash_btn = ctk.CTkButton(
            self,
            text="⚡  Flash Firmware",
            font=ctk.CTkFont(size=16, weight="bold"),
            height=48,
            corner_radius=12,
            fg_color=("#2563eb", "#1d4ed8"),
            hover_color=("#1e40af", "#1e3a8a"),
            command=self._start_flash,
            state="disabled",
        )
        self.flash_btn.grid(row=3, column=0, sticky="ew", padx=24, pady=(16, 0))

        # ── Log output ──
        log_frame = ctk.CTkFrame(self, corner_radius=12)
        log_frame.grid(row=4, column=0, sticky="nsew", padx=24, pady=(16, 0))
        log_frame.grid_columnconfigure(0, weight=1)
        log_frame.grid_rowconfigure(0, weight=1)

        self.log_text = ctk.CTkTextbox(
            log_frame,
            font=ctk.CTkFont(family="Menlo, Consolas, monospace", size=12),
            corner_radius=8,
            wrap="word",
            state="disabled",
        )
        self.log_text.grid(row=0, column=0, sticky="nsew", padx=8, pady=8)

        # ── Status bar ──
        self.status_label = ctk.CTkLabel(
            self,
            text="Ready",
            font=ctk.CTkFont(size=12),
            text_color=("gray50", "gray60"),
            anchor="w",
        )
        self.status_label.grid(row=5, column=0, sticky="ew", padx=28, pady=(8, 16))

    # ── Port management ──

    def _refresh_ports(self):
        ports = get_serial_ports()
        if ports:
            self.port_list = ports
            labels = [label for _, label in ports]
            self.port_menu.configure(values=labels)
            # Auto-select first port if none selected
            if not self.selected_port.get() or self.selected_port.get() == "No ports detected":
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
        """Get the actual device path for the selected port label."""
        label = self.selected_port.get()
        for device, plabel in self.port_list:
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
            self._log("Firmware files found:")
            for name, size in sizes.items():
                self._log(f"  {name}.bin — {size:,} bytes (Flash @ 0x{FLASH_ADDRESSES[name]:05X})")
            self._log("")

    # ── Logging helpers ──

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
        self._set_status("Flashing firmware...")

        self._log("─" * 50)
        self._log(f"Flashing to {port}...")
        self._log("")

        # Run in background thread
        thread = threading.Thread(
            target=self._flash_thread,
            args=(port, files),
            daemon=True,
        )
        thread.start()

    def _flash_thread(self, port, files):
        """Background thread that performs the actual flashing."""
        try:
            import esptool

            # Build esptool command args
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

            self.after(0, self._log, f"esptool args: {' '.join(args[:8])}...")

            # Capture esptool output
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

            # Parse and display output
            output = capture.getvalue()
            for line in output.splitlines():
                line = line.strip()
                if line:
                    self.after(0, self._log, line)

            if success:
                self.after(0, self._log, "")
                self.after(0, self._log, "✅  Firmware flashed successfully!")
                self.after(0, self._set_status, "Flash complete! You can unplug the board.", False, True)
            else:
                self.after(0, self._log, "")
                self.after(0, self._log, "❌  Flash failed! Check the log above.")
                self.after(0, self._set_status, "Flash failed", True, False)

        except ImportError:
            self.after(0, self._log, "❌ esptool not installed! Run: pip install esptool")
            self.after(0, self._set_status, "Error: esptool not found", True, False)
        except Exception as e:
            self.after(0, self._log, f"❌ Unexpected error: {e}")
            self.after(0, self._set_status, f"Error: {e}", True, False)
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
