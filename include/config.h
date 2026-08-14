#pragma once

// Unit Fader connected to the CoreS3 Grove Port B. On that port the white
// Grove wire (the fader's analog output) is GPIO 8. The yellow RGB signal is
// GPIO 9 and drives the fader's two seven-LED rows.
constexpr int FADER_ANALOG_PIN = 8;
constexpr int FADER_RGB_PIN = 9;
constexpr int FADER_LED_COUNT = 14;
constexpr bool FADER_LEDS_REVERSED = false;
// Measured usable travel on this unit. The M5Stack board routes 0 V to the
// physical top and 3.3 V to the physical bottom, so the ADC direction must be
// reversed to make upward travel mean higher volume.
constexpr int FADER_RAW_MIN = 80;
constexpr int FADER_RAW_MAX = 4050;
constexpr bool FADER_REVERSED = true;
