#!/usr/bin/env python3
"""Interactive setup, build, and flash helper for the Sonos ESP32 remote."""

from __future__ import annotations

import argparse
import getpass
import glob
import os
from pathlib import Path
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / "include"
CORE_DIR = ROOT / ".platformio-core"


class Style:
    def __init__(self, enabled: bool) -> None:
        self.enabled = enabled

    def paint(self, code: str, text: str) -> str:
        return f"\033[{code}m{text}\033[0m" if self.enabled else text

    def title(self, text: str) -> str:
        return self.paint("1;36", text)

    def ok(self, text: str) -> str:
        return self.paint("1;32", text)

    def hint(self, text: str) -> str:
        return self.paint("2", text)

    def warn(self, text: str) -> str:
        return self.paint("1;33", text)


def cpp_string(value: str) -> str:
    """Escape a Python string for a one-line C++ string literal."""
    return value.replace("\\", "\\\\").replace('"', '\\"')


def write_private_config(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(content, encoding="utf-8")
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def ask(prompt: str, default: str | None = None) -> str:
    suffix = f" [{default}]" if default is not None else ""
    value = input(f"{prompt}{suffix}: ").strip()
    return value if value else (default or "")


def confirm(prompt: str, default: bool = True) -> bool:
    suffix = "Y/n" if default else "y/N"
    value = input(f"{prompt} [{suffix}]: ").strip().lower()
    if not value:
        return default
    return value in {"y", "yes"}


def configure(style: Style) -> None:
    print(style.title("\n1. Device settings"))
    print(style.hint("Your password is hidden while you type and is never printed."))

    ssid = ask("Wi-Fi name (SSID)")
    while not ssid:
        print(style.warn("The Wi-Fi name cannot be empty."))
        ssid = ask("Wi-Fi name (SSID)")

    password = getpass.getpass("Wi-Fi password: ")
    room = ask("Sonos coordinator room (exact Sonos app spelling)", "Living Room")

    while True:
        maximum = ask("Maximum group volume, 1-100", "40")
        try:
            maximum_number = int(maximum)
            if 1 <= maximum_number <= 100:
                break
        except ValueError:
            pass
        print(style.warn("Enter a whole number from 1 to 100."))

    write_private_config(
        INCLUDE / "wifi_credentials.h",
        "#pragma once\n\n"
        f'constexpr char WIFI_SSID[] = "{cpp_string(ssid)}";\n'
        f'constexpr char WIFI_PASSWORD[] = "{cpp_string(password)}";\n',
    )
    write_private_config(
        INCLUDE / "device_settings.h",
        "#pragma once\n\n"
        f'constexpr char SONOS_COORDINATOR_ROOM[] = "{cpp_string(room)}";\n'
        f"constexpr int SONOS_MAX_VOLUME = {maximum_number};\n",
    )
    print(style.ok("Saved private settings (these files are ignored by Git)."))


def find_platformio() -> list[str] | None:
    local = ROOT / ".venv" / "bin" / "pio"
    if local.is_file():
        return [str(local)]
    pio = shutil.which("pio") or shutil.which("platformio")
    return [pio] if pio else None


def run(command: list[str]) -> int:
    env = os.environ.copy()
    env.setdefault("PLATFORMIO_CORE_DIR", str(CORE_DIR))
    print("\n$ " + " ".join(command))
    return subprocess.run(command, cwd=ROOT, env=env, check=False).returncode


def serial_ports() -> list[str]:
    patterns = [
        "/dev/cu.usbmodem*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/ttyACM*",
        "/dev/ttyUSB*",
    ]
    ports = sorted({port for pattern in patterns for port in glob.glob(pattern)})
    if sys.platform == "win32":
        try:
            from serial.tools import list_ports

            ports.extend(port.device for port in list_ports.comports())
        except ImportError:
            pass
    return sorted(set(ports))


def choose_port(style: Style) -> str | None:
    ports = serial_ports()
    if not ports:
        print(style.warn("No serial port detected; PlatformIO will try automatically."))
        return None
    if len(ports) == 1:
        print(f"Detected: {ports[0]}")
        return ports[0]

    print("Detected serial ports:")
    for number, port in enumerate(ports, start=1):
        print(f"  {number}. {port}")
    while True:
        selected = ask("Port number", "1")
        if selected.isdigit() and 1 <= int(selected) <= len(ports):
            return ports[int(selected) - 1]
        print(style.warn("Choose one of the listed port numbers."))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Configure, build, and flash the Sonos ESP32 remote."
    )
    parser.add_argument(
        "--skip-config", action="store_true", help="keep the existing private settings"
    )
    parser.add_argument(
        "--configure-only", action="store_true", help="write settings, then stop"
    )
    parser.add_argument("--no-color", action="store_true", help="disable ANSI colors")
    args = parser.parse_args()
    style = Style(sys.stdout.isatty() and not args.no_color)

    print(style.title("\n╭──────────────────────────────────╮"))
    print(style.title("│  Sonos ESP32 Remote Setup        │"))
    print(style.title("╰──────────────────────────────────╯"))
    print("This wizard keeps your Wi-Fi password in ignored local files.")

    if not args.skip_config:
        configure(style)
    if args.configure_only:
        return 0

    pio = find_platformio()
    if pio is None:
        print(style.warn("\nPlatformIO is not installed."))
        print("Install it with: python3 -m pip install --user platformio")
        print("Then run this setup again.")
        return 1

    print(style.title("\n2. Build"))
    if not confirm("Build the firmware now?"):
        print("Settings saved. Run the wizard again whenever you are ready.")
        return 0
    if run(pio + ["run"]) != 0:
        print(style.warn("Build failed. Read the error above; nothing was flashed."))
        return 1
    print(style.ok("Build complete."))

    print(style.title("\n3. Flash over USB"))
    if not confirm("Flash the connected CoreS3 now?"):
        return 0
    port = choose_port(style)
    upload = pio + ["run", "--target", "upload"]
    if port:
        upload += ["--upload-port", port]
    if run(upload) != 0:
        print(style.warn("Flash failed. Check that the USB-C cable carries data."))
        return 1
    print(style.ok("Firmware flashed successfully."))

    if confirm("Open the serial monitor?", default=False):
        monitor = pio + ["device", "monitor", "--baud", "115200"]
        if port:
            monitor += ["--port", port]
        return run(monitor)
    print("Done. Press RESET once if the screen does not restart automatically.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
