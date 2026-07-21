#pragma once

#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>

// Geteilt zwischen dem Cube-Modus (V8.ino) und dem HomeSpan-Modus
// (HomeSpanMode.cpp) -- Boot-Wahlschalter und Encoder-Taster.
#define PIN_MODE_SWITCH 20   // LOW = HomeSpan-Modus, HIGH (Pullup) = Cube-Modus
#define PIN_ENC_SW      21

#define BLACK  0x0000
#define WHITE  0xFFFF
#define CX     120
#define CY     120
#define NUM_SIDES  5

extern Adafruit_NeoPixel leds;
extern Arduino_GFX      *gfx;

void     setSideColor(int side, uint32_t color);
void     ledsShow();
uint32_t hsv8ToNeo(uint8_t h, uint8_t s, uint8_t v);
