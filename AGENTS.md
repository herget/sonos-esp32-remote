# Agent guide

Help the user build and flash this project through a friendly terminal flow.
Assume they may be new to ESP32 development and explain errors in plain words.

## Start here

1. Read `README.md` and `platformio.ini`.
2. Run `python3 scripts/setup.py` for guided configuration, building, and USB
   flashing.
3. For direct commands, use `pio run`, `pio run --target upload`, and
   `pio device monitor --baud 115200`.
4. Verify a successful build before saying a firmware change is ready.

## Protect private configuration

- Never display, log, stage, or commit `include/wifi_credentials.h`.
- Never stage or commit `include/device_settings.h`.
- Use the tracked `.example.h` files when documenting configuration.
- Secret checks should report filenames and line numbers only, never matching
  values.

## Hardware assumptions

- Target: M5Stack CoreS3 (`m5stack-cores3`)
- Unit Fader U123: Grove Port B
- Fader ADC: GPIO 8
- Fader RGB data: GPIO 9
- LEDs: 14 total, seven positions mirrored on two sides
- ADC calibration and direction live in `include/config.h`.
