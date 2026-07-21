/*  CubeLED V2
 *
 *  Hardware:
 *    ESP32-C3, GC9A01 240×240 Round TFT (SPI), Rotary Encoder, 80 WS2812B LEDs
 *
 *  LED-Verdrahtung (Snake, 1 Datenpin):
 *    Untere Reihe vorwärts — Seite 0..4 → Index   0.. 39
 *    Obere  Reihe rückwärts — Seite 4..0 → Index  40.. 79
 *
 *  Bibliotheken (Library Manager):
 *    Arduino_GFX_Library  (moononournation)
 *    Adafruit NeoPixel
 *
 *  Encoder: kein ESP32Encoder (PCNT-Hardware) — der ESP32-C3 hat keinen
 *  PCNT-Baustein, die Lib linkt dort nicht (undefined reference). Stattdessen
 *  Software-Quadratur-Decoder per attachInterrupt(), siehe unten.
 */

#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "CubeShared.h"
#include "HomeSpanMode.h"

#define BLACK  0x0000
#define WHITE  0xFFFF

// ═══════════════════════════════════════════════════════════════════
//  PINS
// ═══════════════════════════════════════════════════════════════════
// ESP32-C3 SuperMini: GPIO0-10, 20-21 nutzbar. GPIO12-17 sind intern für den
// Flash reserviert, GPIO18/19 sind natives USB (D-/D+), GPIO9/2/8 sind
// Strapping-Pins (GPIO9 = fest verdrahteter BOOT-Taster) — daher hier gemieden.
#define PIN_TFT_SCK   4
#define PIN_TFT_MOSI  5
#define PIN_TFT_CS    6
#define PIN_TFT_DC    7
#define PIN_TFT_RST   10
#define PIN_NEO       0
#define PIN_ENC_CLK   3
#define PIN_ENC_DT    1
#define PIN_ENC_SW    21

// ═══════════════════════════════════════════════════════════════════
//  PERFORMANCE
// ═══════════════════════════════════════════════════════════════════
#define TARGET_FPS    60
#define FRAME_TIME_MS (1000 / TARGET_FPS)

// ═══════════════════════════════════════════════════════════════════
//  LED LAYOUT
// ═══════════════════════════════════════════════════════════════════
#define NUM_SIDES   5
#define STRIP_LEN   8
#define TOTAL_LEDS  80

Adafruit_NeoPixel leds(TOTAL_LEDS, PIN_NEO, NEO_GRB + NEO_KHZ800);

void setSideColor(int side, uint32_t color) {
  int botBase = side * STRIP_LEN;
  int topBase = 40 + (NUM_SIDES - 1 - side) * STRIP_LEN;
  for (int i = 0; i < STRIP_LEN; i++) {
    leds.setPixelColor(botBase + i, color);
    leds.setPixelColor(topBase + i, color);
  }
}

// ═══════════════════════════════════════════════════════════════════
//  DISPLAY
// ═══════════════════════════════════════════════════════════════════
Arduino_DataBus *bus = new Arduino_HWSPI(PIN_TFT_DC, PIN_TFT_CS,
                                          PIN_TFT_SCK, PIN_TFT_MOSI);
Arduino_GFX    *gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0, true);

#define CX  120
#define CY  120

// ═══════════════════════════════════════════════════════════════════
//  KONFIGURATION (eine globale Farbe für den ganzen Würfel)
// ═══════════════════════════════════════════════════════════════════
struct SideConfig { uint8_t hue, sat, bri; };

SideConfig cfg     = {0, 255, 200};
SideConfig cfgPrev;
bool prevEditMode = false;

// ═══════════════════════════════════════════════════════════════════
//  UI STATE
// ═══════════════════════════════════════════════════════════════════
enum Setting { SETTING_BRI = 0, SETTING_HUE, SETTING_SAT,
               SETTING_EFFECT, NUM_SETTINGS };

const char* SETTING_NAMES[NUM_SETTINGS] = {
  "HELLIGKEIT", "FARBE", "SAETTIGUNG", "EFFEKT"
};

int  curSetting = SETTING_BRI;
bool editMode   = false;

bool needFullRedraw   = true;
bool needArcRedraw    = false;
bool needDotsRedraw   = false;
bool needBorderRedraw = false;

// ═══════════════════════════════════════════════════════════════════
//  EFFEKTE (global — laufen über den ganzen Würfel, nicht pro Seite)
// ═══════════════════════════════════════════════════════════════════
enum Effect { EFFECT_OFF = 0, EFFECT_RAINBOW, EFFECT_WAVE, EFFECT_BREATHE,
              EFFECT_COMET, EFFECT_LAVA, NUM_EFFECTS };

const char* EFFECT_NAMES[NUM_EFFECTS] = {
  "AUS", "REGENBOGEN", "FARBWELLE", "ATMEN", "COMET", "LAVALAMPE"
};

int     curEffect       = EFFECT_OFF;
uint8_t effectSpeed      = 128;
uint8_t prevEffectSpeed  = 128;
bool    speedEditMode    = false;  // zweite Editier-Stufe: Effekt wählen vs. Geschwindigkeit einstellen
float   effectPhase      = 0.0f;
uint8_t effectPreviewHue = 0;
uint8_t effectPreviewSat = 255;
int     prevActiveEffect = -1;

// ═══════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════
unsigned long lastFrameMs = 0;

// ═══════════════════════════════════════════════════════════════════
//  ENCODER (Software-Vollquadratur mit Gray-Code-Übergangstabelle)
// ═══════════════════════════════════════════════════════════════════
volatile long  encoderCount = 0;
long           lastCount    = 0;

// ═══════════════════════════════════════════════════════════════════
//  TASTER (entprellt über Zustands-Stabilität, nicht über reine Zeitfenster
//  seit dem letzten Druck — sonst löst ein kurzes Prellen mitten im
//  physischen Tastendruck 30ms später einen zweiten, ungewollten Klick aus)
// ═══════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS 30
bool          btnRawLast  = HIGH;
bool          btnStable   = HIGH;
unsigned long btnChangeMs = 0;

// Gray-Code-Übergangstabelle (Standard-Vollschritt-Dekodierung): Index ist
// (alter AB-Zustand << 2 | neuer AB-Zustand), Wert ist +1/-1 für einen
// echten Einzelschritt oder 0 für einen ungültigen Sprung (Prellen oder
// verpasste Zwischenflanke). Dadurch braucht es kein Zeitfenster-Debounce
// mehr — Prellen hebt sich durch die Zustandsprüfung von selbst auf, statt
// als Zählschritt in die falsche Richtung zu landen (das Risiko bei reinem
// Ein-Kanal-Decoding mit Zeitfenster-Verwerfung).
static const int8_t QUAD_TABLE[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};
volatile uint8_t lastAB = 0;

void IRAM_ATTR handleEncoderISR() {
  uint8_t a  = digitalRead(PIN_ENC_CLK);
  uint8_t b  = digitalRead(PIN_ENC_DT);
  uint8_t ab = (a << 1) | b;
  encoderCount += QUAD_TABLE[(lastAB << 2) | ab];
  lastAB = ab;
}

// ═══════════════════════════════════════════════════════════════════
//  PERSISTENZ
// ═══════════════════════════════════════════════════════════════════
Preferences   prefs;
bool          savePending = false;
unsigned long lastEditMs  = 0;
bool          ledsDirty   = false;

// ═══════════════════════════════════════════════════════════════════
//  FARB-HELPER
// ═══════════════════════════════════════════════════════════════════

uint32_t hsv8ToNeo(uint8_t h, uint8_t s, uint8_t v) {
  return Adafruit_NeoPixel::ColorHSV((uint16_t)h * 257, s, v);
}

uint16_t hsv8To565(uint8_t h, uint8_t s, uint8_t v) {
  uint32_t c = hsv8ToNeo(h, s, v);
  return gfx->color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

uint16_t dim565(uint16_t c, float f) {
  uint8_t r = ((c >> 11) & 0x1F) * f;
  uint8_t g = ((c >> 5)  & 0x3F) * f;
  uint8_t b = ( c        & 0x1F) * f;
  return (r << 11) | (g << 5) | b;
}

// ═══════════════════════════════════════════════════════════════════
//  TRIG-TABELLE (ESP32-C3 hat keine Hardware-FPU — sinf/cosf sind
//  Software-Routinen; 1°-Tabelle ersetzt sie durch Array-Zugriffe)
// ═══════════════════════════════════════════════════════════════════
float sinTable[360];
float cosTable[360];

void initTrigTables() {
  for (int i = 0; i < 360; i++) {
    float rad = i * (float)M_PI / 180.0f;
    sinTable[i] = sinf(rad);
    cosTable[i] = cosf(rad);
  }
}

static inline int degToIndex(float deg) {
  int i = (int)(deg >= 0.0f ? deg + 0.5f : deg - 0.5f);
  i %= 360;
  if (i < 0) i += 360;
  return i;
}

static inline float fastSinDeg(float deg) { return sinTable[degToIndex(deg)]; }
static inline float fastCosDeg(float deg) { return cosTable[degToIndex(deg)]; }

static inline float wrapDeg360(float d) {
  while (d < 0.0f)   d += 360.0f;
  while (d >= 360.0f) d -= 360.0f;
  return d;
}

// ═══════════════════════════════════════════════════════════════════
//  ZEICHEN-PRIMITIVES
// ═══════════════════════════════════════════════════════════════════

void fillArcSector(int cx, int cy, int innerR, int outerR,
                   float startDeg, float endDeg, uint16_t color) {
  if (innerR >= outerR) return;

  float eDeg = endDeg;
  if (eDeg <= startDeg) eDeg += 360.0f;

  const float STEP = 1.0f;  // 1° für glatte Kanten

  for (float d = startDeg; d < eDeg; d += STEP) {
    float d2 = d + STEP;
    if (d2 > eDeg) d2 = eDeg;

    int ox0 = cx + (int)(outerR * fastSinDeg(d));
    int oy0 = cy - (int)(outerR * fastCosDeg(d));
    int ox1 = cx + (int)(outerR * fastSinDeg(d2));
    int oy1 = cy - (int)(outerR * fastCosDeg(d2));
    int ix0 = cx + (int)(innerR * fastSinDeg(d));
    int iy0 = cy - (int)(innerR * fastCosDeg(d));
    int ix1 = cx + (int)(innerR * fastSinDeg(d2));
    int iy1 = cy - (int)(innerR * fastCosDeg(d2));

    gfx->fillTriangle(ox0, oy0, ox1, oy1, ix0, iy0, color);
    gfx->fillTriangle(ox1, oy1, ix1, iy1, ix0, iy0, color);
  }
}

void drawArcMarker(int cx, int cy, int midR, float angleDeg,
                   uint16_t outerCol, uint16_t innerCol) {
  int mx = cx + (int)(midR * fastSinDeg(angleDeg));
  int my = cy - (int)(midR * fastCosDeg(angleDeg));
  gfx->fillCircle(mx, my, 7, outerCol);
  gfx->fillCircle(mx, my, 5, innerCol);
}

void drawCentered(const char* text, int y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setTextWrap(false);
  int16_t w = strlen(text) * 6 * size;
  gfx->setCursor(CX - w / 2, y);
  gfx->print(text);
}

// ═══════════════════════════════════════════════════════════════════
//  RING-REPARATUR: alten Marker übermalen ohne den Ring zu löschen
// ═══════════════════════════════════════════════════════════════════

// Stellt den Regenbogenring im ±6°-Bereich um markerDeg wieder her
void repairHueRing(float markerDeg) {
  for (float d = markerDeg - 6.0f; d <= markerDeg + 6.0f; d += 1.0f) {
    float wrapped = wrapDeg360(d);
    uint8_t h8 = (uint8_t)((uint32_t)(int)wrapped * 255 / 360);
    fillArcSector(CX, CY, 86, 108, d, d + 1.0f, hsv8To565(h8, 255, 200));
  }
}

// Stellt den Sättigungs-Gradientring im ±6°-Bereich wieder her
// (auf 225°..495° begrenzt, da außerhalb kein Ring existiert — sonst
// malt das Reparatur-Fenster nahe sat=0/255 Farbe in die Lücke am Rand)
void repairSatRing(float markerDeg, uint8_t hue) {
  float from = constrain(markerDeg - 6.0f, 225.0f, 495.0f);
  float to   = constrain(markerDeg + 6.0f, 225.0f, 495.0f);
  for (float d = from; d < to; d += 1.0f) {
    float d2 = d + 1.0f;
    if (d2 > to) d2 = to;
    float p = constrain((d - 225.0f) / 270.0f, 0.0f, 1.0f);
    uint8_t s = (uint8_t)(p * 255.0f);
    fillArcSector(CX, CY, 86, 108, d, d2, hsv8To565(hue, s, 200));
  }
  // Endpunkt-Markierungen wiederherstellen falls übermalt
  if (markerDeg - 6.0f <= 228.0f) fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
  if (markerDeg + 6.0f >= 492.0f) fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);
}

// ═══════════════════════════════════════════════════════════════════
//  GEMEINSAME UI-ELEMENTE
// ═══════════════════════════════════════════════════════════════════

void clearScreenZones() {
  // Deckt den gesamten Innenbereich bis knapp vor den Edit-Rand (117/118) ab,
  // damit keine Ring-/Marker-Reste des vorherigen Screens stehen bleiben.
  gfx->fillCircle(CX, CY, 116, BLACK);
  gfx->fillRect(60, 208, 120, 18, BLACK);
}

// Eckenradius der Titel-Box — von drawTitle() (fillRoundRect) UND
// fillCircleExceptRoundRect() (Aussparung) verwendet, damit beide Formen
// exakt übereinstimmen.
const int16_t TITLE_BOX_RADIUS = 6;

// Berechnet den Rahmen der Titel-Box für den aktuellen Menüpunkt (gleiche
// Formel wie in drawTitle) — wird auch von fillCircleExceptRoundRect()
// gebraucht, damit der Vorschau-Kreis exakt weiß, welchen Bereich er
// aussparen muss.
void titleBoxRect(int16_t &boxX, int16_t &boxY, int16_t &boxW, int16_t &boxH) {
  const char* text  = SETTING_NAMES[curSetting];
  int16_t     textW = strlen(text) * 6 * 2;
  boxW = textW + 14;
  boxH = 24;
  boxX = CX - boxW / 2;
  boxY = 96;
}

// Menüname — bei allen vier Screens an derselben Stelle/Größe. Da der
// Vorschau-Kreis jetzt wieder ein Vollkreis ist, liegt der Titel bei
// Farbe/Sättigung direkt über dessen farbiger Fläche — deshalb ein
// abgerundeter Kasten dahinter, der ~40% der Hintergrundfarbe durchscheinen
// lässt (dim565 auf 0.4), damit der weiße Text lesbar bleibt. bgColor ist
// BLACK bei Helligkeit (kein Kreis dort → Kasten bleibt unsichtbar,
// da dim565(BLACK,...) wieder BLACK ergibt).
// Nur bei needFullRedraw nötig: der Name ändert sich nur, wenn curSetting
// wechselt, und genau dann wurde der Bereich gerade von clearScreenZones()
// geleert. Bei Farbe/Sättigung spart der Vorschau-Kreis diesen Bereich jetzt
// aktiv aus (siehe fillCircleExceptRoundRect), daher bleibt die Box auch bei
// Hue-/Sat-Änderungen unangetastet — kein Redraw pro Tick nötig, kein
// Flackern durch wiederholtes Übermalen von Box+Text.
void drawTitle(uint16_t bgColor) {
  if (!needFullRedraw) return;

  int16_t boxX, boxY, boxW, boxH;
  titleBoxRect(boxX, boxY, boxW, boxH);

  gfx->fillRoundRect(boxX, boxY, boxW, boxH, TITLE_BOX_RADIUS, dim565(bgColor, 0.4f));
  drawCentered(SETTING_NAMES[curSetting], 100, 2, WHITE);
}

// Für eine Zeile (Abstand dy von der nächsten Ecken-Mitte, 0 wenn im
// geraden Mittelteil) liefert dies den Einzug von rx bzw. rx+rw, den die
// abgerundete Ecke an dieser Zeile gegenüber dem vollen Rechteck hat.
static inline int16_t roundRectInset(int16_t dy, int16_t rr) {
  if (dy <= 0) return 0;
  if (dy >= rr) return rr;
  return rr - (int16_t)sqrtf((float)(rr * rr - dy * dy));
}

// Füllt einen Vollkreis, spart dabei aber die abgerundete Titel-Box aus,
// statt sie zu überschreiben — die Aussparung folgt exakt der Form von
// fillRoundRect() (inkl. Eckenradius), damit an den Ecken keine schwarzen
// Keile stehen bleiben. So kann der Vorschau-Kreis bei jeder Hue-/
// Sat-Änderung neu gefüllt werden, ohne je den darüberliegenden Titeltext
// zu berühren — der Text muss dadurch nur einmal gezeichnet werden und
// flackert nicht mehr.
void fillCircleExceptRoundRect(int cx, int cy, int r, uint16_t color,
                               int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                               int16_t rr) {
  int16_t topCenterY    = ry + rr;
  int16_t bottomCenterY = ry + rh - rr;

  for (int16_t y = -r; y <= r; y++) {
    int16_t dx   = (int16_t)sqrtf((float)(r * r - (int)y * (int)y));
    int16_t rowY = cy + y;
    int16_t x0   = cx - dx;
    int16_t x1   = cx + dx;

    if (rowY >= ry && rowY < ry + rh) {
      int16_t dy = 0;
      if (rowY < topCenterY)         dy = topCenterY - rowY;
      else if (rowY >= bottomCenterY) dy = rowY - bottomCenterY + 1;
      int16_t inset = roundRectInset(dy, rr);
      int16_t left  = rx + inset;
      int16_t right = rx + rw - inset;

      if (x0 < left)   gfx->drawFastHLine(x0, rowY, left - x0, color);
      if (x1 + 1 > right) gfx->drawFastHLine(right, rowY, x1 + 1 - right, color);
    } else {
      gfx->drawFastHLine(x0, rowY, x1 - x0 + 1, color);
    }
  }
}

// Menü-Anzeige: ein Punkt pro Einstellungsseite (HELLIGKEIT, FARBE,
// SAETTIGUNG, EFFEKT), der aktive Punkt entspricht curSetting — grau statt
// farbig. Ändert sich nur mit curSetting, siehe drawTitle().
void drawMenuDots() {
  if (!needFullRedraw) return;

  int startX = CX - (NUM_SETTINGS - 1) * 14 / 2;
  for (int i = 0; i < NUM_SETTINGS; i++) {
    int x = startX + i * 14;
    gfx->fillCircle(x, 218, 4, BLACK);
    if (i == curSetting) {
      gfx->fillCircle(x, 218, 4, 0xC618);  // helles Grau
    } else {
      gfx->fillCircle(x, 218, 2, 0x2104);  // dunkles Grau
    }
  }
}

void drawEditBorder() {
  if (!needBorderRedraw && editMode == prevEditMode) return;

  if (editMode) {
    uint16_t c = hsv8To565(cfg.hue, 255, 220);
    gfx->drawCircle(CX, CY, 118, c);
    gfx->drawCircle(CX, CY, 117, dim565(c, 0.35f));
  } else {
    gfx->drawCircle(CX, CY, 118, BLACK);
    gfx->drawCircle(CX, CY, 117, BLACK);
  }
  prevEditMode = editMode;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: HELLIGKEIT
// ═══════════════════════════════════════════════════════════════════
void drawBrightnessScreen() {
  if (needFullRedraw) clearScreenZones();

  uint8_t  bri   = cfg.bri;
  uint16_t sCol  = hsv8To565(cfg.hue, cfg.sat, 200);
  uint16_t empty = 0x18C3;

  bool briChanged = (cfgPrev.bri != bri);
  bool hueChanged = (cfgPrev.hue != cfg.hue);
  bool satChanged = (cfgPrev.sat != cfg.sat);

  if (needFullRedraw || hueChanged || satChanged) {
    // Vollständiger Bogen (Screen-/Farbwechsel)
    fillArcSector(CX, CY, 86, 108, 225.0f, 495.0f, empty);
    if (bri > 0) {
      fillArcSector(CX, CY, 86, 108, 225.0f, 225.0f + 270.0f * (bri / 255.0f), sCol);
    }
    fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
    fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);

  } else if (needArcRedraw || briChanged) {
    // Nur das Delta zwischen altem und neuem Helligkeitswert neu zeichnen
    float oldDeg = 225.0f + 270.0f * (cfgPrev.bri / 255.0f);
    float newDeg = 225.0f + 270.0f * (bri / 255.0f);
    if (newDeg > oldDeg) {
      fillArcSector(CX, CY, 86, 108, oldDeg, newDeg, sCol);
    } else if (newDeg < oldDeg) {
      fillArcSector(CX, CY, 86, 108, newDeg, oldDeg, empty);
    }
  }

  if (needFullRedraw || hueChanged || satChanged || briChanged) {
    // Prozentwert — unter dem Titel (Titel endet bei y=100+16=116)
    gfx->fillRect(CX - 50, 120, 100, 30, BLACK);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (bri * 100) / 255);
    drawCentered(buf, 122, 3, WHITE);
  }

  drawTitle(BLACK);
  drawMenuDots();
  drawEditBorder();
  cfgPrev = cfg;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: FARBE (HUE)
// ═══════════════════════════════════════════════════════════════════
void drawHueScreen() {
  bool hueChanged = (cfgPrev.hue != cfg.hue);
  bool satChanged = (cfgPrev.sat != cfg.sat);

  if (needFullRedraw) {
    clearScreenZones();

    // Regenbogenring komplett zeichnen
    for (int deg = 0; deg < 360; deg++) {
      uint8_t h8 = (uint8_t)((uint32_t)deg * 255 / 360);
      fillArcSector(CX, CY, 86, 108, (float)deg, (float)(deg + 2),
                    hsv8To565(h8, 255, 200));
    }
    float markerDeg = (float)cfg.hue * 360.0f / 255.0f;
    drawArcMarker(CX, CY, 97, markerDeg,
                  WHITE, hsv8To565(cfg.hue, 255, 230));

  } else if (needArcRedraw || hueChanged) {
    // Alter Marker gezielt übermalen (nur ±6° des Rings neu zeichnen)
    float oldDeg = (float)cfgPrev.hue * 360.0f / 255.0f;
    repairHueRing(oldDeg);

    float newDeg = (float)cfg.hue * 360.0f / 255.0f;
    drawArcMarker(CX, CY, 97, newDeg,
                  WHITE, hsv8To565(cfg.hue, 255, 230));
  }

  uint16_t previewCol = hsv8To565(cfg.hue, cfg.sat, 200);
  if (needFullRedraw || needArcRedraw || hueChanged || satChanged) {
    int16_t boxX, boxY, boxW, boxH;
    titleBoxRect(boxX, boxY, boxW, boxH);
    fillCircleExceptRoundRect(CX, CY, 58, previewCol, boxX, boxY, boxW, boxH, TITLE_BOX_RADIUS);
  }

  drawTitle(previewCol);
  drawMenuDots();
  drawEditBorder();
  cfgPrev = cfg;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: SAETTIGUNG
// ═══════════════════════════════════════════════════════════════════
void drawSaturationScreen() {
  uint8_t hue = cfg.hue;
  uint8_t sat = cfg.sat;

  bool satChanged = (cfgPrev.sat != sat);
  bool hueChanged = (cfgPrev.hue != hue);

  if (needFullRedraw) clearScreenZones();

  if (needFullRedraw || hueChanged) {
    // Gradient-Ring komplett zeichnen (bei fullRedraw oder Farbwechsel)
    const int SEG = 60;
    for (int i = 0; i < SEG; i++) {
      uint8_t s  = (uint8_t)((uint32_t)i * 255 / SEG);
      float   sA = 225.0f + (float)i * 270.0f / SEG;
      float   eA = 225.0f + (float)(i + 1) * 270.0f / SEG + 0.5f;
      fillArcSector(CX, CY, 86, 108, sA, eA, hsv8To565(hue, s, 200));
    }
    fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
    fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);

    float markerDeg = 225.0f + (sat / 255.0f) * 270.0f;
    drawArcMarker(CX, CY, 97, markerDeg, WHITE, hsv8To565(hue, sat, 230));

  } else if (needArcRedraw || satChanged) {
    // Alter Marker gezielt übermalen
    float oldDeg = 225.0f + (cfgPrev.sat / 255.0f) * 270.0f;
    repairSatRing(oldDeg, hue);

    float newDeg = 225.0f + (sat / 255.0f) * 270.0f;
    drawArcMarker(CX, CY, 97, newDeg, WHITE, hsv8To565(hue, sat, 230));
  }

  uint16_t previewCol = hsv8To565(hue, sat, 200);
  if (needFullRedraw || needArcRedraw || satChanged || hueChanged) {
    int16_t boxX, boxY, boxW, boxH;
    titleBoxRect(boxX, boxY, boxW, boxH);
    fillCircleExceptRoundRect(CX, CY, 58, previewCol, boxX, boxY, boxW, boxH, TITLE_BOX_RADIUS);
  }

  drawTitle(previewCol);
  drawMenuDots();
  drawEditBorder();
  cfgPrev = cfg;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: EFFEKT
// ═══════════════════════════════════════════════════════════════════
// 270°-Bereich (225°..495°) mit 90°-Lücke unten — dadurch kann kein Sektor
// bis zur Menü-Anzeige/Titel-Box durchreichen, ganz ohne Sonderbehandlung
// dort. Sektoren sind neutral grau statt farbig, da ein Effekt keine eigene
// feste Farbe hat (die Farbe kommt zur Laufzeit von cfg).
const float SIDE_START = 225.0f;
const float SIDE_END   = 495.0f;
const float SIDE_GAP   = 5.0f;
const float EFFECT_SWEEP = ((SIDE_END - SIDE_START) - NUM_EFFECTS * SIDE_GAP) / NUM_EFFECTS;

float effectSectorStart(int i) { return SIDE_START + i * (EFFECT_SWEEP + SIDE_GAP); }

void drawEffectSector(int i, bool active) {
  float startDeg = effectSectorStart(i);
  float endDeg   = startDeg + EFFECT_SWEEP;

  fillArcSector(CX, CY, 80, 108, startDeg, endDeg, BLACK);

  uint16_t c = active ? 0xC618 /* helles Grau */ : 0x2104 /* dunkles Grau */;
  int outerR = active ? 108 : 103;
  int innerR = active ? 80  :  87;
  fillArcSector(CX, CY, innerR, outerR, startDeg, endDeg, c);

  float midDeg = startDeg + EFFECT_SWEEP / 2.0f;
  int   tx     = CX + (int)(62 * fastSinDeg(midDeg)) - 4;
  int   ty     = CY - (int)(62 * fastCosDeg(midDeg)) - 6;
  gfx->setTextSize(1);
  gfx->setTextColor(active ? BLACK : 0x8410);
  gfx->setCursor(tx, ty);
  gfx->print(i + 1);
}

// Geschwindigkeits-Bogen — gleiche Mechanik wie der Helligkeits-Bogen
// (voller Redraw vs. Delta-Redraw zwischen altem/neuem Wert).
void drawEffectSpeedArc(bool full) {
  uint16_t col   = (curEffect == EFFECT_OFF) ? 0x2104
                  : hsv8To565(effectPreviewHue, effectPreviewSat, 200);
  uint16_t empty = 0x18C3;

  if (full) {
    fillArcSector(CX, CY, 86, 108, 225.0f, 495.0f, empty);
    if (effectSpeed > 0) {
      fillArcSector(CX, CY, 86, 108, 225.0f, 225.0f + 270.0f * (effectSpeed / 255.0f), col);
    }
    fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
    fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);
  } else {
    float oldDeg = 225.0f + 270.0f * (prevEffectSpeed / 255.0f);
    float newDeg = 225.0f + 270.0f * (effectSpeed / 255.0f);
    if (newDeg > oldDeg)      fillArcSector(CX, CY, 86, 108, oldDeg, newDeg, col);
    else if (newDeg < oldDeg) fillArcSector(CX, CY, 86, 108, newDeg, oldDeg, empty);
  }
  prevEffectSpeed = effectSpeed;
}

// Wie fillCircleExceptRoundRect(), spart zusätzlich noch die Zeile mit dem
// Effektnamen (unter dem Titel) komplett aus — dort steht Text mit
// variabler Breite, ein einfaches volles Aussparen der Zeile ist einfacher
// als eine zweite exakte Breitenberechnung und optisch gleichwertig.
void fillEffectPreviewCircle(uint16_t color) {
  int16_t boxX, boxY, boxW, boxH;
  titleBoxRect(boxX, boxY, boxW, boxH);
  const int16_t nameY = 124, nameH = 20;

  int16_t topCenterY    = boxY + TITLE_BOX_RADIUS;
  int16_t bottomCenterY = boxY + boxH - TITLE_BOX_RADIUS;
  const int r = 58;

  for (int16_t y = -r; y <= r; y++) {
    int16_t dx   = (int16_t)sqrtf((float)(r * r - (int)y * (int)y));
    int16_t rowY = CY + y;
    int16_t x0   = CX - dx;
    int16_t x1   = CX + dx;

    if (rowY >= nameY && rowY < nameY + nameH) continue;

    if (rowY >= boxY && rowY < boxY + boxH) {
      int16_t dy = 0;
      if (rowY < topCenterY)          dy = topCenterY - rowY;
      else if (rowY >= bottomCenterY) dy = rowY - bottomCenterY + 1;
      int16_t inset = roundRectInset(dy, TITLE_BOX_RADIUS);
      int16_t left  = boxX + inset;
      int16_t right = boxX + boxW - inset;
      if (x0 < left)      gfx->drawFastHLine(x0, rowY, left - x0, color);
      if (x1 + 1 > right) gfx->drawFastHLine(right, rowY, x1 + 1 - right, color);
    } else {
      gfx->drawFastHLine(x0, rowY, x1 - x0 + 1, color);
    }
  }
}

void drawEffectScreen() {
  bool effectChanged = (curEffect != prevActiveEffect);

  if (needFullRedraw) {
    clearScreenZones();
    if (speedEditMode) {
      drawEffectSpeedArc(true);
    } else {
      for (int i = 0; i < NUM_EFFECTS; i++) drawEffectSector(i, i == curEffect);
    }

  } else if (speedEditMode) {
    if (needArcRedraw) drawEffectSpeedArc(false);

  } else if (needArcRedraw && effectChanged) {
    drawEffectSector(prevActiveEffect, false);
    drawEffectSector(curEffect, true);
  }

  // Vorschau-Kreis läuft, solange ein Effekt aktiv ist, jeden Frame mit —
  // das Aussparen von Titel- und Namenszone verhindert dabei jedes
  // Übermalen von Text (siehe fillEffectPreviewCircle).
  uint16_t previewCol = (curEffect == EFFECT_OFF)
      ? 0x2104
      : hsv8To565(effectPreviewHue, effectPreviewSat, 200);
  fillEffectPreviewCircle(previewCol);

  if (needFullRedraw || effectChanged) {
    gfx->fillRect(CX - 76, 124, 152, 20, BLACK);
    drawCentered(EFFECT_NAMES[curEffect], 126, 2, WHITE);
  }

  drawTitle(previewCol);
  drawMenuDots();
  drawEditBorder();

  prevActiveEffect = curEffect;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN DISPATCHER
// ═══════════════════════════════════════════════════════════════════
void drawScreen() {
  switch (curSetting) {
    case SETTING_BRI:    drawBrightnessScreen(); break;
    case SETTING_HUE:    drawHueScreen();        break;
    case SETTING_SAT:    drawSaturationScreen(); break;
    case SETTING_EFFECT: drawEffectScreen();     break;
  }

  needFullRedraw   = false;
  needArcRedraw    = false;
  needDotsRedraw   = false;
  needBorderRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
//  LEDS
// ═══════════════════════════════════════════════════════════════════

// Setzt die Ausgabe der WS2812-Daten ab, während die Encoder-ISR pausiert
// ist — sonst kann ein GPIO-Interrupt (CLK/DT feuern bei aktivem Drehen sehr
// häufig) mitten in die zeitkritische RMT-Übertragung fallen und einzelne
// LEDs für einen Frame mit Zufallsfarben zeigen ("Flackern beim Farbe
// ändern", genau dann wenn Drehen + show() gleichzeitig passieren).
void ledsShow() {
  detachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK));
  detachInterrupt(digitalPinToInterrupt(PIN_ENC_DT));
  leds.show();
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  handleEncoderISR, CHANGE);
}

void updateLeds() {
  uint32_t c = leds.gamma32(hsv8ToNeo(cfg.hue, cfg.sat, cfg.bri));
  for (int s = 0; s < NUM_SIDES; s++) {
    setSideColor(s, c);
  }
  ledsShow();
}

// ═══════════════════════════════════════════════════════════════════
//  EFFEKT-ANIMATION
// ═══════════════════════════════════════════════════════════════════
// Effekte sind global (ein Effekt für den ganzen Würfel) und benutzen die
// aktuelle Farbe/Helligkeit (cfg) als Basis — so passt sich z.B. die
// Lavalampe an die zuletzt eingestellte Farbe an.

void advanceEffectPhase() {
  // effectSpeed 0..255 → ca. 0.05..3.0 Grad/Frame (bei 60 FPS ~3..180 Grad/s)
  float degPerFrame = 0.05f + (effectSpeed / 255.0f) * 3.0f;
  effectPhase = wrapDeg360(effectPhase + degPerFrame);
}

void applyEffectLeds() {
  uint8_t baseHue = cfg.hue;
  uint8_t baseSat = cfg.sat;
  uint8_t baseBri = cfg.bri;

  switch (curEffect) {

    case EFFECT_RAINBOW: {
      // Feiner, durchgehender Regenbogen über alle 80 LEDs, rotiert mit effectPhase
      for (int i = 0; i < TOTAL_LEDS; i++) {
        float deg = wrapDeg360(effectPhase + i * 360.0f / TOTAL_LEDS);
        uint8_t h = (uint8_t)((uint32_t)degToIndex(deg) * 255 / 360);
        leds.setPixelColor(i, leds.gamma32(hsv8ToNeo(h, 255, baseBri)));
      }
      effectPreviewHue = (uint8_t)((uint32_t)degToIndex(effectPhase) * 255 / 360);
      effectPreviewSat = 255;
      break;
    }

    case EFFECT_WAVE: {
      // Grober Regenbogen in 5 Blöcken (einer pro Seite), wandert seitenweise
      for (int s = 0; s < NUM_SIDES; s++) {
        float deg = wrapDeg360(effectPhase + s * 360.0f / NUM_SIDES);
        uint8_t h = (uint8_t)((uint32_t)degToIndex(deg) * 255 / 360);
        setSideColor(s, leds.gamma32(hsv8ToNeo(h, 255, baseBri)));
      }
      effectPreviewHue = (uint8_t)((uint32_t)degToIndex(effectPhase) * 255 / 360);
      effectPreviewSat = 255;
      break;
    }

    case EFFECT_BREATHE: {
      // Helligkeit pulsiert sinusförmig in der aktuellen Farbe
      float briF = (fastSinDeg(effectPhase) + 1.0f) * 0.5f;  // 0..1
      uint8_t bri = (uint8_t)(briF * baseBri);
      uint32_t c = leds.gamma32(hsv8ToNeo(baseHue, baseSat, bri));
      for (int s = 0; s < NUM_SIDES; s++) setSideColor(s, c);
      effectPreviewHue = baseHue;
      effectPreviewSat = baseSat;
      break;
    }

    case EFFECT_COMET: {
      // Heller Kopf mit ausblendendem Schweif läuft um den Würfel
      uint32_t bg = leds.gamma32(hsv8ToNeo(baseHue, baseSat, baseBri / 6));
      for (int i = 0; i < TOTAL_LEDS; i++) leds.setPixelColor(i, bg);

      const int TAIL = 10;
      int headPos = ((int)(effectPhase / 360.0f * TOTAL_LEDS)) % TOTAL_LEDS;
      for (int t = 0; t <= TAIL; t++) {
        int idx = (headPos - t + TOTAL_LEDS) % TOTAL_LEDS;
        float f = 1.0f - (float)t / TAIL;
        uint8_t b = (uint8_t)(baseBri * f);
        leds.setPixelColor(idx, leds.gamma32(hsv8ToNeo(baseHue, baseSat, b)));
      }
      effectPreviewHue = baseHue;
      effectPreviewSat = baseSat;
      break;
    }

    case EFFECT_LAVA: {
      // Organische „Blobs“ aus zwei überlagerten Sinuswellen je LED —
      // moduliert Helligkeit und leicht den Hue um die Basisfarbe herum.
      for (int i = 0; i < TOTAL_LEDS; i++) {
        float p1 = wrapDeg360(effectPhase * 0.6f  + i * 9.0f);
        float p2 = wrapDeg360(effectPhase * 0.37f - i * 5.0f + 60.0f);
        float v  = (fastSinDeg(p1) + fastSinDeg(p2)) * 0.5f;      // -1..1
        float briF = 0.35f + 0.65f * ((v + 1.0f) * 0.5f);          // 0.35..1
        uint8_t bri = (uint8_t)(briF * baseBri);
        uint8_t h   = (uint8_t)(baseHue + (int8_t)(v * 10.0f));
        leds.setPixelColor(i, leds.gamma32(hsv8ToNeo(h, baseSat, bri)));
        if (i == 0) { effectPreviewHue = h; effectPreviewSat = baseSat; }
      }
      break;
    }

    default: break;  // EFFECT_OFF wird nie hier behandelt (siehe loop())
  }

  ledsShow();
}

// Wählt je nach aktuellem Effekt-Zustand die richtige LED-Ausgabe —
// von setup() genutzt, damit ein beim Start geladener aktiver Effekt
// sofort animiert statt (einmalig) als statisches Bild angezeigt zu werden.
void refreshLeds() {
  if (curEffect == EFFECT_OFF) updateLeds();
  else                         applyEffectLeds();
}

// ═══════════════════════════════════════════════════════════════════
//  SAVE / LOAD
// ═══════════════════════════════════════════════════════════════════
void saveConfig() {
  prefs.begin("cube", false);
  prefs.putUChar("h", cfg.hue);
  prefs.putUChar("s", cfg.sat);
  prefs.putUChar("b", cfg.bri);
  prefs.putUChar("fx",  (uint8_t)curEffect);
  prefs.putUChar("fxs", effectSpeed);
  prefs.end();
}

void loadConfig() {
  prefs.begin("cube", true);
  cfg.hue = prefs.getUChar("h", cfg.hue);
  cfg.sat = prefs.getUChar("s", cfg.sat);
  cfg.bri = prefs.getUChar("b", cfg.bri);
  curEffect   = prefs.getUChar("fx",  (uint8_t)curEffect);
  effectSpeed = prefs.getUChar("fxs", effectSpeed);
  prefs.end();
  cfgPrev          = cfg;
  prevActiveEffect = curEffect;
  prevEffectSpeed  = effectSpeed;
}

// ═══════════════════════════════════════════════════════════════════
//  ENCODER-HANDLING
// ═══════════════════════════════════════════════════════════════════
void handleEncoder() {
  long count = encoderCount / 4;  // 4 gültige Übergänge pro Rastung (Vollquadratur)
  int  delta = (int)(count - lastCount);
  if (delta == 0) return;
  lastCount = count;

  if (!editMode) {
    curSetting     = (curSetting + delta + NUM_SETTINGS) % NUM_SETTINGS;
    needFullRedraw = true;
  } else {
    switch (curSetting) {
      case SETTING_BRI:
        cfg.bri = (uint8_t)constrain((int)cfg.bri + delta * 5, 0, 255);
        needArcRedraw = true;
        break;
      case SETTING_HUE:
        cfg.hue = (uint8_t)((cfg.hue + delta * 3 + 256) % 256);
        needArcRedraw  = true;
        needDotsRedraw = true;
        break;
      case SETTING_SAT:
        cfg.sat = (uint8_t)constrain((int)cfg.sat + delta * 5, 0, 255);
        needArcRedraw  = true;
        needDotsRedraw = true;
        break;
      case SETTING_EFFECT:
        if (speedEditMode) {
          effectSpeed = (uint8_t)constrain((int)effectSpeed + delta * 5, 0, 255);
        } else {
          curEffect = (curEffect + delta + NUM_EFFECTS) % NUM_EFFECTS;
        }
        needArcRedraw = true;
        break;
    }
    ledsDirty   = true;
    savePending = true;
    lastEditMs  = millis();
  }
}

void handleButton() {
  bool raw = digitalRead(PIN_ENC_SW);

  if (raw != btnRawLast) {
    btnChangeMs = millis();  // jede Pegeländerung startet das Entprell-Fenster neu
    btnRawLast  = raw;
  }
  if (millis() - btnChangeMs < BTN_DEBOUNCE_MS) return;  // noch nicht stabil (Prellen)
  if (raw == btnStable) return;   // stabiler Zustand hat sich nicht geändert
  btnStable = raw;
  if (btnStable != LOW) return;   // nur auf den Druck reagieren, nicht auf das Loslassen

  // Effekt-Screen hat zwei Editier-Stufen hinter einem Tastendruck:
  // 1. Druck → editMode an, Encoder wählt den Effekt (wie das Seiten-Rad).
  // 2. Druck → speedEditMode an, Encoder stellt jetzt die Geschwindigkeit ein.
  // 3. Druck → beides wieder aus, zurück zur Menü-Navigation.
  if (curSetting == SETTING_EFFECT && editMode && !speedEditMode) {
    speedEditMode  = true;
    needFullRedraw = true;  // wechselt die Ring-Darstellung von Rad auf Bogen
    return;
  }

  editMode         = !editMode;
  speedEditMode    = false;
  needBorderRedraw = true;
  if (curSetting == SETTING_EFFECT) needFullRedraw = true;  // Rad/Bogen-Zustand zurücksetzen
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════
void cubeSetup() {
  Serial.begin(115200);

  initTrigTables();

  gfx->begin();
  gfx->fillScreen(BLACK);

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT,  INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_DT),  handleEncoderISR, CHANGE);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  leds.begin();
  leds.setBrightness(255);
  leds.show();

  loadConfig();
  refreshLeds();
  needFullRedraw = true;
  lastFrameMs    = millis();
}

void cubeLoop() {
  unsigned long now = millis();

  handleButton();
  handleEncoder();

  if (now - lastFrameMs >= FRAME_TIME_MS) {
    lastFrameMs = now;

    bool animating = (curEffect != EFFECT_OFF);
    if (animating) {
      advanceEffectPhase();
      applyEffectLeds();
    } else if (ledsDirty) {
      updateLeds();
      ledsDirty = false;
    }

    // Der Effekt-Screen animiert seine Vorschau live mit, unabhängig von
    // Encoder-/Tasten-Eingaben — deshalb hier zusätzlich zu den normalen
    // Redraw-Flags erzwungen neu zeichnen, solange er sichtbar ist.
    bool needsScreenAnim = (curSetting == SETTING_EFFECT && animating);

    if (needFullRedraw || needArcRedraw ||
        needDotsRedraw || needBorderRedraw || needsScreenAnim) {
      drawScreen();
    }
  }

  if (savePending && millis() - lastEditMs > 3000) {
    saveConfig();
    savePending = false;
  }
}

// ═══════════════════════════════════════════════════════════════════
//  BOOT-MODUSWAHL: Cube-Modus vs. HomeSpan/HomeKit-Modus
// ═══════════════════════════════════════════════════════════════════
// GPIO20 wird einmalig beim Boot gelesen -- ein Umlegen des Schalters
// wirkt erst nach einem Reset/Neustart, kein Live-Wechsel im Betrieb.
static bool homeSpanModeActive = false;

void setup() {
  pinMode(PIN_MODE_SWITCH, INPUT_PULLUP);
  homeSpanModeActive = (digitalRead(PIN_MODE_SWITCH) == LOW);

  if (homeSpanModeActive) homeSpanModeSetup();
  else                    cubeSetup();
}

void loop() {
  if (homeSpanModeActive) homeSpanModeLoop();
  else                    cubeLoop();
}
