# CubeLED

A small LED cube lamp: 80 addressable LEDs inside a 3D-printed cube, controlled with a **round TFT display** and a **rotary encoder**. It can also be used as an **Apple HomeKit** lamp.

## Features

- **Local control:** turn the encoder to pick a setting (brightness, color, saturation, effect), press to edit it. A round 240×240 display shows the current value as an arc.
- **Effects:** Rainbow, color wave, breathing, comet, lava lamp, each with adjustable speed.
- **HomeKit mode (V8):** a toggle switch picks the mode at boot. In HomeKit mode the cube shows up as a color light in Apple Home (via [HomeSpan](https://github.com/HomeSpan/HomeSpan)). During onboarding the display shows WiFi setup steps and a pairing QR code.
- Settings are saved to flash and restored after a restart.
- Runs at about 60 FPS.

## Hardware

| Part | Details |
|---|---|
| MCU | ESP32-C3 SuperMini |
| Display | GC9A01 240×240 round TFT (SPI) |
| LEDs | 80× WS2812B (5 sides × 2 rows × 8 LEDs, one data pin) |
| Input | Rotary encoder with push button |
| Mode switch | GPIO20 (GND = HomeKit, open = cube mode) |
| PCB | Custom KiCad board (`KiCad/`) |

## Repository structure

```
Arduino/
  V1..V8/             Firmware versions (V8 = latest, with HomeKit)
  BrightnessControl/  Early display + brightness test
  SmartDial/          Encoder/dial UI prototype
  docs/               Design spec and plan for the HomeKit boot switch
KiCad/CubeLED/        Schematic and PCB
```

## Getting started

1. Install these libraries from the Arduino Library Manager: **Arduino_GFX_Library**, **Adafruit NeoPixel**, **HomeSpan**.
2. Board: *ESP32C3 Dev Module*.
3. Open `Arduino/V8/V8.ino` and upload it.
4. HomeKit mode: flip the switch, restart the cube, connect it to WiFi and scan the QR code on the display with the Home app.
