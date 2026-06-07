/*
 * BrightnessControl
 * ESP32 + GC9A01 240x240 + WS2812B + Rotary Encoder
 *
 * Benötigte Libraries (Library Manager):
 *   Arduino_GFX_Library  (moononournation)
 *   FastLED
 *   Adafruit GFX Library  (für den Font)
 */

#include <Arduino_GFX_Library.h>
#include <FastLED.h>
#include "FreeSansBold18pt7b.h"

// ── Pins ─────────────────────────────────────────────────────────────
#define PIN_LED       13
#define NUM_LEDS      80
#define PIN_TFT_DC     4
#define PIN_TFT_CS    17
#define PIN_TFT_SCK   18
#define PIN_TFT_MOSI  23
#define PIN_TFT_RST   16
#define PIN_ENC_A     25
#define PIN_ENC_B     26
#define PIN_ENC_BTN   27

// ── Display ──────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI);
Arduino_GFX    *gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0, true);

#define CX  120
#define CY  120

// ── LEDs ─────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

// ── Zustand ──────────────────────────────────────────────────────────
int  brightness = 80;
int  prevBri    = -1;   // -1 = noch nicht gezeichnet
bool needDraw   = true;

// ── Encoder ISR ──────────────────────────────────────────────────────
portMUX_TYPE  mux    = portMUX_INITIALIZER_UNLOCKED;
volatile int  encD   = 0;
volatile byte lastAB = 0;

void IRAM_ATTR encISR() {
  byte a = digitalRead(PIN_ENC_A), b = digitalRead(PIN_ENC_B);
  byte ab = (a << 1) | b, sm = (lastAB << 2) | ab;
  if (sm == 0b1101 || sm == 0b0100 || sm == 0b0010 || sm == 0b1011) {
    portENTER_CRITICAL_ISR(&mux); encD++; portEXIT_CRITICAL_ISR(&mux);
  }
  if (sm == 0b1110 || sm == 0b0111 || sm == 0b0001 || sm == 0b1000) {
    portENTER_CRITICAL_ISR(&mux); encD--; portEXIT_CRITICAL_ISR(&mux);
  }
  lastAB = ab;
}

// ── Farb-Helfer ──────────────────────────────────────────────────────
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

uint16_t lerp565(uint16_t ca, uint16_t cb, float t) {
  int ar = (ca >> 11) & 0x1F, ag = (ca >> 5) & 0x3F, ab_ = ca & 0x1F;
  int br = (cb >> 11) & 0x1F, bg = (cb >> 5) & 0x3F, bb_ = cb & 0x1F;
  return (uint16_t)(ar + t * (br - ar) + 0.5f) << 11 |
         (uint16_t)(ag + t * (bg - ag) + 0.5f) << 5  |
         (uint16_t)(ab_ + t * (bb_ - ab_) + 0.5f);
}


// ── Arc-Konfiguration ────────────────────────────────────────────────
// 0° = 12-Uhr, Uhrzeigersinn
// Bogen: 225° (unten-links) → 495° (unten-rechts), 270° Sweep
#define A_START  225.0f
#define A_SPAN   270.0f
#define A_END    (A_START + A_SPAN)
#define A_OUT    107.0f
#define A_IN      88.0f

// Absoluter Gradient: Farbe nur abhängig von Winkelposition, nicht vom Indikator.
// Ermöglicht inkrementelles Update ohne Vollneuzeichnung.
uint16_t g_trackCol;

uint16_t arcColorAt(float a) {
  float p = (a - A_START) / A_SPAN;
  return lerp565(rgb565(50, 50, 50), 0xFFFF, p);
}

// Zeichnet einen Arc-Sektor lückenlos mit zwei Dreiecken.
// roundf statt (int) vermeidet systematische Randpixel-Auslassungen.
// +1/-1 Puffer auf den Radien stellt sicher, dass keine Pixelreste aus
// vorherigen Frames stehen bleiben.
void fillArcSeg(float a0, float a1, float ro, float ri, uint16_t col) {
  float r0 = a0 * DEG_TO_RAD, r1 = a1 * DEG_TO_RAD;
  int ox0 = CX + (int)roundf(sinf(r0) * ro), oy0 = CY - (int)roundf(cosf(r0) * ro);
  int ox1 = CX + (int)roundf(sinf(r1) * ro), oy1 = CY - (int)roundf(cosf(r1) * ro);
  int ix0 = CX + (int)roundf(sinf(r0) * ri), iy0 = CY - (int)roundf(cosf(r0) * ri);
  int ix1 = CX + (int)roundf(sinf(r1) * ri), iy1 = CY - (int)roundf(cosf(r1) * ri);
  gfx->fillTriangle(ox0, oy0, ox1, oy1, ix0, iy0, col);
  gfx->fillTriangle(ox1, oy1, ix1, iy1, ix0, iy0, col);
}

// Zeichnet einen Winkelbereich neu mit korrekter Farbe (aktiv oder Track).
void redrawArcRange(float aLo, float aHi, float activeUpTo) {
  aLo = max(aLo, A_START);
  aHi = min(aHi, A_END);
  for (float a = floorf(aLo); a < aHi; a += 1.0f) {
    float a2  = a + 1.0f; if (a2 > aHi) a2 = aHi;
    float mid = (a + a2) * 0.5f;
    uint16_t col = (mid <= activeUpTo) ? arcColorAt(mid) : g_trackCol;
    fillArcSeg(a, a2, A_OUT + 1, A_IN - 1, col);
  }
}

// ── Arc ──────────────────────────────────────────────────────────────
void drawArc(int bri) {
  // Auf ganzen Grad runden: verhindert partielle (<1°) Segmente an der Grenze,
  // die beim Erase-Pass ein anderes Dreieck-Raster erzeugen als beim Draw-Pass.
  float newInd  = roundf(A_START + (bri           / 100.0f) * A_SPAN);
  float prevInd = roundf(A_START + (max(prevBri, 0) / 100.0f) * A_SPAN);

  if (prevBri < 0) {
    // Erster Frame: gesamten Bogen einmal zeichnen
    redrawArcRange(A_START, A_END, newInd);
  } else {
    // Inkrementell: nur geänderter Bereich + 8° Puffer um den alten Dot
    const float DOT_BUF = 8.0f;
    float lo = min(prevInd - DOT_BUF, min(prevInd, newInd));
    float hi = max(prevInd + DOT_BUF, max(prevInd, newInd));
    redrawArcRange(lo, hi, newInd);
  }

  // End-Caps an festen Positionen (immer neuzeichnen)
  fillArcSeg(A_START - 2.0f, A_START + 2.0f, A_OUT + 1, A_IN - 1, rgb565(28, 28, 28));
  fillArcSeg(A_END   - 2.0f, A_END   + 2.0f, A_OUT + 1, A_IN - 1, rgb565(28, 28, 28));

  // Indikator-Dot: kein fillCircle für Hintergrund (würde außerhalb der Arc-Band
  // malen und Reste hinterlassen). redrawArcRange hat den Bereich bereits korrekt gesetzt.
  float rad  = newInd * DEG_TO_RAD;
  float midR = (A_OUT + A_IN) * 0.5f;
  int   dx   = CX + (int)roundf(sinf(rad) * midR);
  int   dy   = CY - (int)roundf(cosf(rad) * midR);
  // Kein drawCircle (nur Outline) – einzelne Outline-Pixel können durch die
  // Triangle-Fill-Rule in fillArcSeg nicht abgedeckt werden und bleiben hängen.
  gfx->fillCircle(dx, dy, 5, rgb565(12, 12, 12));  // gefüllter dunkler Hintergrund
  gfx->fillCircle(dx, dy, 4, 0xFFFF);

  prevBri = bri;
}

// ── Text ─────────────────────────────────────────────────────────────
void drawLabel() {
  gfx->setFont(nullptr);
  gfx->setTextSize(1);
  gfx->setTextColor(rgb565(90, 90, 90), 0x0000);
  gfx->setTextWrap(false);
  const char *label = "Helligkeit";
  int w = strlen(label) * 6;
  gfx->setCursor(CX - w / 2, CY - 34);
  gfx->print(label);
}

void drawPercentage(int bri) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", bri);

  gfx->setFont(&FreeSansBold18pt7b);
  gfx->setTextColor(0xFFFF, 0x0000);
  gfx->setTextWrap(false);

  int16_t  x1, y1;
  uint16_t tw, th;
  gfx->getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
  gfx->setCursor(CX - x1 - tw / 2, CY + 16);
  gfx->print(buf);
  gfx->setFont(nullptr);
}

// ── LEDs ─────────────────────────────────────────────────────────────
void updateLEDs(int bri) {
  uint8_t v = map(bri, 0, 100, 0, 255);
  fill_solid(leds, NUM_LEDS, CRGB::White);
  FastLED.setBrightness(v);
  FastLED.show();
}

// ── Render ───────────────────────────────────────────────────────────
void renderFrame(int bri) {
  drawArc(bri);
  // Nur Text-Rechteck löschen (liegt bei r<75 vom Zentrum, überlappt nie mit Bogen)
  gfx->fillRect(CX - 55, CY - 38, 110, 62, 0x0000);
  drawLabel();
  drawPercentage(bri);
}

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  g_trackCol = rgb565(20, 20, 20);

  gfx->begin(80000000);
  gfx->fillScreen(0x0000);
  gfx->drawCircle(CX, CY, 118, rgb565(38, 38, 38));
  gfx->drawCircle(CX, CY, 119, rgb565(22, 22, 22));

  pinMode(PIN_ENC_A,   INPUT_PULLUP);
  pinMode(PIN_ENC_B,   INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);

  updateLEDs(brightness);
  renderFrame(brightness);
}

// ── Loop ─────────────────────────────────────────────────────────────
void loop() {
  int d = 0;
  portENTER_CRITICAL(&mux); d = encD; encD = 0; portEXIT_CRITICAL(&mux);

  if (d) {
    brightness = constrain(brightness + d, 0, 100);
    updateLEDs(brightness);
    needDraw = true;
  }

  if (needDraw) {
    needDraw = false;
    renderFrame(brightness);
  }
}
