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
 *    ESP32Encoder
 */

#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <ESP32Encoder.h>
#include <Preferences.h>

#define BLACK  0x0000
#define WHITE  0xFFFF

// ═══════════════════════════════════════════════════════════════════
//  PINS
// ═══════════════════════════════════════════════════════════════════
#define PIN_TFT_SCK   18
#define PIN_TFT_MOSI  23
#define PIN_TFT_CS    17
#define PIN_TFT_DC    4
#define PIN_TFT_RST   16
#define PIN_NEO       13
#define PIN_ENC_CLK   25
#define PIN_ENC_DT    26
#define PIN_ENC_SW    27

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
//  KONFIGURATION
// ═══════════════════════════════════════════════════════════════════
struct SideConfig { uint8_t hue, sat, bri; };

SideConfig cfg[NUM_SIDES] = {
  {0,   255, 200},
  {51,  255, 200},
  {102, 255, 200},
  {153, 255, 200},
  {204, 255, 200}
};

SideConfig cfgPrev[NUM_SIDES];
bool prevEditMode = false;

// ═══════════════════════════════════════════════════════════════════
//  UI STATE
// ═══════════════════════════════════════════════════════════════════
enum Setting { SETTING_BRI = 0, SETTING_HUE, SETTING_SAT, SETTING_SIDE,
               NUM_SETTINGS };

const char* SETTING_NAMES[NUM_SETTINGS] = {
  "HELLIGKEIT", "FARBE", "SAETTIGUNG", "SEITE"
};

int  curSetting = SETTING_BRI;
int  curSide    = 0;
bool editMode   = false;

bool needFullRedraw   = true;
bool needArcRedraw    = false;
bool needDotsRedraw   = false;
bool needBorderRedraw = false;

// ═══════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════
unsigned long lastFrameMs = 0;

// ═══════════════════════════════════════════════════════════════════
//  ENCODER
// ═══════════════════════════════════════════════════════════════════
ESP32Encoder  encoder;
long          lastCount = 0;
unsigned long lastBtnMs = 0;

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

// Menüname — bei allen vier Screens an derselben Stelle/Größe. Da der
// Vorschau-Kreis jetzt wieder ein Vollkreis ist, liegt der Titel bei
// Farbe/Sättigung direkt über dessen farbiger Fläche — deshalb ein
// abgerundeter Kasten dahinter, der ~40% der Hintergrundfarbe durchscheinen
// lässt (dim565 auf 0.4), damit der weiße Text lesbar bleibt. bgColor ist
// BLACK bei Helligkeit/Seite (kein Kreis dort → Kasten bleibt unsichtbar,
// da dim565(BLACK,...) wieder BLACK ergibt).
// Nur bei needFullRedraw nötig: der Name ändert sich nur, wenn curSetting
// wechselt, und genau dann wurde der Bereich gerade von clearScreenZones()
// geleert. Das Seiten-Wheel hat jetzt dieselbe Lücke wie die anderen Ringe
// und kann diesen Bereich nicht mehr erreichen, daher kein Redraw pro Tick
// nötig (vermeidet sichtbares Flackern).
void drawTitle(uint16_t bgColor) {
  if (!needFullRedraw) return;

  const char* text  = SETTING_NAMES[curSetting];
  int16_t     textW = strlen(text) * 6 * 2;
  int16_t     boxW  = textW + 14;
  int16_t     boxH  = 24;
  int16_t     boxX  = CX - boxW / 2;
  int16_t     boxY  = 96;

  gfx->fillRoundRect(boxX, boxY, boxW, boxH, 6, dim565(bgColor, 0.4f));
  drawCentered(text, 100, 2, WHITE);
}

// Seiten-/Menü-Anzeige: ein Punkt pro Einstellungsseite (HELLIGKEIT, FARBE,
// SAETTIGUNG, SEITE), der aktive Punkt entspricht curSetting — grau statt
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
    uint16_t c = hsv8To565(cfg[curSide].hue, 255, 220);
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

  uint8_t  bri   = cfg[curSide].bri;
  uint16_t sCol  = hsv8To565(cfg[curSide].hue, cfg[curSide].sat, 200);
  uint16_t empty = 0x18C3;

  bool briChanged = (cfgPrev[curSide].bri != bri);
  bool hueChanged = (cfgPrev[curSide].hue != cfg[curSide].hue);
  bool satChanged = (cfgPrev[curSide].sat != cfg[curSide].sat);

  if (needFullRedraw || hueChanged || satChanged) {
    // Vollständiger Bogen (Screen-/Seiten-/Farbwechsel)
    fillArcSector(CX, CY, 86, 108, 225.0f, 495.0f, empty);
    if (bri > 0) {
      fillArcSector(CX, CY, 86, 108, 225.0f, 225.0f + 270.0f * (bri / 255.0f), sCol);
    }
    fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
    fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);

  } else if (needArcRedraw || briChanged) {
    // Nur das Delta zwischen altem und neuem Helligkeitswert neu zeichnen
    float oldDeg = 225.0f + 270.0f * (cfgPrev[curSide].bri / 255.0f);
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
  cfgPrev[curSide] = cfg[curSide];
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: FARBE (HUE)
// ═══════════════════════════════════════════════════════════════════
void drawHueScreen() {
  bool hueChanged = (cfgPrev[curSide].hue != cfg[curSide].hue);
  bool satChanged = (cfgPrev[curSide].sat != cfg[curSide].sat);

  if (needFullRedraw) {
    clearScreenZones();

    // Regenbogenring komplett zeichnen
    for (int deg = 0; deg < 360; deg++) {
      uint8_t h8 = (uint8_t)((uint32_t)deg * 255 / 360);
      fillArcSector(CX, CY, 86, 108, (float)deg, (float)(deg + 2),
                    hsv8To565(h8, 255, 200));
    }
    float markerDeg = (float)cfg[curSide].hue * 360.0f / 255.0f;
    drawArcMarker(CX, CY, 97, markerDeg,
                  WHITE, hsv8To565(cfg[curSide].hue, 255, 230));

  } else if (needArcRedraw || hueChanged) {
    // Alter Marker gezielt übermalen (nur ±6° des Rings neu zeichnen)
    float oldDeg = (float)cfgPrev[curSide].hue * 360.0f / 255.0f;
    repairHueRing(oldDeg);

    float newDeg = (float)cfg[curSide].hue * 360.0f / 255.0f;
    drawArcMarker(CX, CY, 97, newDeg,
                  WHITE, hsv8To565(cfg[curSide].hue, 255, 230));
  }

  uint16_t previewCol = hsv8To565(cfg[curSide].hue, cfg[curSide].sat, 200);
  if (needFullRedraw || needArcRedraw || hueChanged || satChanged) {
    gfx->fillCircle(CX, CY, 58, previewCol);
  }

  drawTitle(previewCol);
  drawMenuDots();
  drawEditBorder();
  cfgPrev[curSide] = cfg[curSide];
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: SAETTIGUNG
// ═══════════════════════════════════════════════════════════════════
void drawSaturationScreen() {
  uint8_t hue = cfg[curSide].hue;
  uint8_t sat = cfg[curSide].sat;

  bool satChanged = (cfgPrev[curSide].sat != sat);
  bool hueChanged = (cfgPrev[curSide].hue != hue);

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
    float oldDeg = 225.0f + (cfgPrev[curSide].sat / 255.0f) * 270.0f;
    repairSatRing(oldDeg, hue);

    float newDeg = 225.0f + (sat / 255.0f) * 270.0f;
    drawArcMarker(CX, CY, 97, newDeg, WHITE, hsv8To565(hue, sat, 230));
  }

  uint16_t previewCol = hsv8To565(hue, sat, 200);
  if (needFullRedraw || needArcRedraw || satChanged || hueChanged) {
    gfx->fillCircle(CX, CY, 58, previewCol);
  }

  drawTitle(previewCol);
  drawMenuDots();
  drawEditBorder();
  cfgPrev[curSide] = cfg[curSide];
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: SEITE
// ═══════════════════════════════════════════════════════════════════
// Gleicher 270°-Bereich (225°..495°) wie bei Helligkeit/Sättigung, mit
// derselben 90°-Lücke unten — dadurch kann kein Sektor mehr bis zur
// Menü-Anzeige/Titel-Box durchreichen, ganz ohne Sonderbehandlung dort.
const float SIDE_START = 225.0f;
const float SIDE_END   = 495.0f;
const float SIDE_GAP   = 5.0f;
const float SIDE_SWEEP = ((SIDE_END - SIDE_START) - NUM_SIDES * SIDE_GAP) / NUM_SIDES;
int prevActiveSide = -1;  // merkt sich den zuletzt aktiven Sektor für die Wheel-Reparatur

float sideSectorStart(int i) { return SIDE_START + i * (SIDE_SWEEP + SIDE_GAP); }

void drawSideSector(int i, bool active) {
  float startDeg = sideSectorStart(i);
  float endDeg   = startDeg + SIDE_SWEEP;

  // Kompletten möglichen Radialbereich (aktiv ODER inaktiv) erst löschen,
  // damit beim Wechsel keine Ringreste des jeweils anderen Zustands bleiben.
  fillArcSector(CX, CY, 80, 108, startDeg, endDeg, BLACK);

  uint16_t c = active
    ? hsv8To565(cfg[i].hue, cfg[i].sat, 220)
    : dim565(hsv8To565(cfg[i].hue, cfg[i].sat, 200), 0.2f);

  int outerR = active ? 108 : 103;
  int innerR = active ? 80  :  87;
  fillArcSector(CX, CY, innerR, outerR, startDeg, endDeg, c);

  float midDeg = startDeg + SIDE_SWEEP / 2.0f;
  int   tx     = CX + (int)(62 * fastSinDeg(midDeg)) - 4;
  int   ty     = CY - (int)(62 * fastCosDeg(midDeg)) - 6;
  gfx->setTextSize(1);
  gfx->setTextColor(active ? WHITE : 0x4208);
  gfx->setCursor(tx, ty);
  gfx->print(i + 1);
}

void drawSideScreen() {
  if (needFullRedraw) {
    clearScreenZones();
    for (int i = 0; i < NUM_SIDES; i++) {
      drawSideSector(i, i == curSide);
    }
    prevActiveSide = curSide;

  } else if (needArcRedraw && curSide != prevActiveSide) {
    // Nur den alten (jetzt inaktiven) und neuen (jetzt aktiven) Sektor neu zeichnen
    drawSideSector(prevActiveSide, false);
    drawSideSector(curSide, true);
    prevActiveSide = curSide;
  }

  // Aktive Seitennummer — unter dem Titel (Titel endet bei y=100+16=116)
  gfx->fillRect(CX - 30, 120, 60, 38, BLACK);
  char buf[3];
  snprintf(buf, sizeof(buf), "%d", curSide + 1);
  drawCentered(buf, 122, 4, WHITE);

  drawTitle(BLACK);
  drawMenuDots();
  drawEditBorder();
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN DISPATCHER
// ═══════════════════════════════════════════════════════════════════
void drawScreen() {
  switch (curSetting) {
    case SETTING_BRI:  drawBrightnessScreen(); break;
    case SETTING_HUE:  drawHueScreen();        break;
    case SETTING_SAT:  drawSaturationScreen(); break;
    case SETTING_SIDE: drawSideScreen();       break;
  }

  needFullRedraw   = false;
  needArcRedraw    = false;
  needDotsRedraw   = false;
  needBorderRedraw = false;
}

// ═══════════════════════════════════════════════════════════════════
//  LEDS
// ═══════════════════════════════════════════════════════════════════
void updateLeds() {
  for (int s = 0; s < NUM_SIDES; s++) {
    uint32_t c = leds.gamma32(hsv8ToNeo(cfg[s].hue, cfg[s].sat, cfg[s].bri));
    setSideColor(s, c);
  }
  leds.show();
}

// ═══════════════════════════════════════════════════════════════════
//  SAVE / LOAD
// ═══════════════════════════════════════════════════════════════════
void saveConfig() {
  prefs.begin("cube", false);
  for (int i = 0; i < NUM_SIDES; i++) {
    char k[4];
    snprintf(k, sizeof(k), "h%d", i); prefs.putUChar(k, cfg[i].hue);
    snprintf(k, sizeof(k), "s%d", i); prefs.putUChar(k, cfg[i].sat);
    snprintf(k, sizeof(k), "b%d", i); prefs.putUChar(k, cfg[i].bri);
  }
  prefs.end();
}

void loadConfig() {
  prefs.begin("cube", true);
  for (int i = 0; i < NUM_SIDES; i++) {
    char k[4];
    snprintf(k, sizeof(k), "h%d", i); cfg[i].hue = prefs.getUChar(k, cfg[i].hue);
    snprintf(k, sizeof(k), "s%d", i); cfg[i].sat = prefs.getUChar(k, cfg[i].sat);
    snprintf(k, sizeof(k), "b%d", i); cfg[i].bri = prefs.getUChar(k, cfg[i].bri);
  }
  prefs.end();
  for (int i = 0; i < NUM_SIDES; i++) cfgPrev[i] = cfg[i];
}

// ═══════════════════════════════════════════════════════════════════
//  ENCODER-HANDLING
// ═══════════════════════════════════════════════════════════════════
void handleEncoder() {
  long count = encoder.getCount() / 2;
  int  delta = (int)(count - lastCount);
  if (delta == 0) return;
  lastCount = count;

  if (!editMode) {
    curSetting     = (curSetting + delta + NUM_SETTINGS) % NUM_SETTINGS;
    needFullRedraw = true;
  } else {
    switch (curSetting) {
      case SETTING_BRI:
        cfg[curSide].bri = (uint8_t)constrain((int)cfg[curSide].bri + delta * 5, 0, 255);
        needArcRedraw = true;
        break;
      case SETTING_HUE:
        cfg[curSide].hue = (uint8_t)((cfg[curSide].hue + delta * 3 + 256) % 256);
        needArcRedraw  = true;
        needDotsRedraw = true;
        break;
      case SETTING_SAT:
        cfg[curSide].sat = (uint8_t)constrain((int)cfg[curSide].sat + delta * 5, 0, 255);
        needArcRedraw  = true;
        needDotsRedraw = true;
        break;
      case SETTING_SIDE:
        curSide       = (curSide + delta + NUM_SIDES) % NUM_SIDES;
        needArcRedraw = true;  // nur betroffene Sektoren neu zeichnen
        break;
    }
    ledsDirty   = true;
    savePending = true;
    lastEditMs  = millis();
  }
}

void handleButton() {
  if (digitalRead(PIN_ENC_SW) != LOW) return;
  if (millis() - lastBtnMs < 250) return;
  lastBtnMs = millis();

  editMode         = !editMode;
  needBorderRedraw = true;
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  initTrigTables();

  gfx->begin();
  gfx->fillScreen(BLACK);

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(PIN_ENC_CLK, PIN_ENC_DT);
  encoder.setCount(0);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  leds.begin();
  leds.setBrightness(255);
  leds.show();

  loadConfig();
  updateLeds();
  needFullRedraw = true;
  lastFrameMs    = millis();
}

void loop() {
  unsigned long now = millis();

  handleButton();
  handleEncoder();

  if (now - lastFrameMs >= FRAME_TIME_MS) {
    lastFrameMs = now;
    if (ledsDirty) {
      updateLeds();
      ledsDirty = false;
    }
    if (needFullRedraw || needArcRedraw ||
        needDotsRedraw || needBorderRedraw) {
      drawScreen();
    }
  }

  if (savePending && millis() - lastEditMs > 3000) {
    saveConfig();
    savePending = false;
  }
}
