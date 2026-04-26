/*  CubeLED V2 - OPTIMIZED
 *
 *  Hardware:
 *    ESP32-C3, GC9A01 240×240 Round TFT (SPI), Rotary Encoder, 80 WS2812B LEDs
 *
 *  LED-Verdrahtung (Snake, 1 Datenpin):
 *    Untere Reihe vorwärts — Seite 0..4 → Index   0.. 39
 *    Obere  Reihe rückwärts — Seite 4..0 → Index  40.. 79
 *
 *  OPTIMIERUNGEN:
 *    ✓ Partielles Neuzeichnen (nur geänderte Elemente)
 *    ✓ Effizientere Arc-Berechnungen
 *    ✓ Display-Caching
 *    ✓ Timing-basierte Updates (60 FPS Target)
 *    ✓ Dirty-Region-Tracking
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
#define PIN_TFT_SCK   18   // VSPI CLK
#define PIN_TFT_MOSI  23   // VSPI MOSI
#define PIN_TFT_CS    17
#define PIN_TFT_DC    4
#define PIN_TFT_RST   16
#define PIN_NEO       13
#define PIN_ENC_CLK   25
#define PIN_ENC_DT    26
#define PIN_ENC_SW    27

// ═══════════════════════════════════════════════════════════════════
//  PERFORMANCE KONFIGURATION
// ═══════════════════════════════════════════════════════════════════
#define TARGET_FPS           60
#define FRAME_TIME_MS        (1000 / TARGET_FPS)
#define ENCODER_DEBOUNCE_MS  50

// ═══════════════════════════════════════════════════════════════════
//  LED LAYOUT
// ═══════════════════════════════════════════════════════════════════
#define NUM_SIDES   5
#define STRIP_LEN   8     // LEDs pro Strip
#define TOTAL_LEDS  80    // 5 × 2 × 8

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

// Speichere vorherige Werte zum Vergleich
SideConfig cfgPrev[NUM_SIDES];
int prevSetting = -1;
int prevSide = -1;
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

// Flags für gezieltes Neuzeichnen
bool needFullRedraw = true;
bool needArcRedraw = false;
bool needHeaderRedraw = false;
bool needDotsRedraw = false;
bool needBorderRedraw = false;

// ═══════════════════════════════════════════════════════════════════
//  TIMING
// ═══════════════════════════════════════════════════════════════════
unsigned long lastFrameMs = 0;
unsigned long lastEncoderMs = 0;

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

// ═══════════════════════════════════════════════════════════════════
//  LOOKUP-TABLES für Performance (Pre-computed)
// ═══════════════════════════════════════════════════════════════════
static float sinLUT[360];
static float cosLUT[360];

void initTrigTables() {
  for (int i = 0; i < 360; i++) {
    float rad = i * M_PI / 180.0f;
    sinLUT[i] = sinf(rad);
    cosLUT[i] = cosf(rad);
  }
}

inline float fastSin(int deg) {
  deg = deg % 360;
  if (deg < 0) deg += 360;
  return sinLUT[deg];
}

inline float fastCos(int deg) {
  deg = deg % 360;
  if (deg < 0) deg += 360;
  return cosLUT[deg];
}

// ═══════════════════════════════════════════════════════════════════
//  FARB-HELPER
// ═══════════════════════════════════════════════════════════════════

// 8-bit HSV → NeoPixel 32-bit
uint32_t hsv8ToNeo(uint8_t h, uint8_t s, uint8_t v) {
  return Adafruit_NeoPixel::ColorHSV((uint16_t)h * 257, s, v);
}

// 8-bit HSV → RGB565
uint16_t hsv8To565(uint8_t h, uint8_t s, uint8_t v) {
  uint32_t c = hsv8ToNeo(h, s, v);
  return gfx->color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// RGB565 abdunkeln (f: 0.0=schwarz, 1.0=unverändert)
uint16_t dim565(uint16_t c, float f) {
  uint8_t r = ((c >> 11) & 0x1F) * f;
  uint8_t g = ((c >> 5)  & 0x3F) * f;
  uint8_t b = ( c        & 0x1F) * f;
  return (r << 11) | (g << 5) | b;
}

// ═══════════════════════════════════════════════════════════════════
//  ZEICHEN-PRIMITIVES (OPTIMIERT)
// ═══════════════════════════════════════════════════════════════════

// Schnellere Arc-Zeichnung mit größeren Schritten
void fillArcSector(int cx, int cy, int innerR, int outerR,
                   float startDeg, float endDeg, uint16_t color) {
  if (innerR >= outerR) return;

  float sRad = startDeg * (float)M_PI / 180.0f;
  float eRad = endDeg   * (float)M_PI / 180.0f;
  if (eRad <= sRad) eRad += 2.0f * (float)M_PI;

  // Adaptive Schrittweite basierend auf Außenradius
  const float STEP = 2.0f * (float)M_PI / 180.0f;  // 2° pro Schritt statt 1°

  for (float a = sRad; a < eRad; a += STEP) {
    float a2 = a + STEP;
    if (a2 > eRad) a2 = eRad;

    int ox0 = cx + (int)(outerR * sinf(a));
    int oy0 = cy - (int)(outerR * cosf(a));
    int ox1 = cx + (int)(outerR * sinf(a2));
    int oy1 = cy - (int)(outerR * cosf(a2));
    int ix0 = cx + (int)(innerR * sinf(a));
    int iy0 = cy - (int)(innerR * cosf(a));
    int ix1 = cx + (int)(innerR * sinf(a2));
    int iy1 = cy - (int)(innerR * cosf(a2));

    gfx->fillTriangle(ox0, oy0, ox1, oy1, ix0, iy0, color);
    gfx->fillTriangle(ox1, oy1, ix1, iy1, ix0, iy0, color);
  }
}

// Kreismarker auf dem Ring-Mittelradius
void drawArcMarker(int cx, int cy, int midR, float angleDeg,
                   uint16_t outerCol, uint16_t innerCol) {
  float rad = angleDeg * (float)M_PI / 180.0f;
  int mx = cx + (int)(midR * sinf(rad));
  int my = cy - (int)(midR * cosf(rad));
  gfx->fillCircle(mx, my, 7, outerCol);
  gfx->fillCircle(mx, my, 5, innerCol);
}

// Zentrierter Text
void drawCentered(const char* text, int y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setTextWrap(false);
  int16_t w = strlen(text) * 6 * size;
  gfx->setCursor(CX - w / 2, y);
  gfx->print(text);
}

// ═══════════════════════════════════════════════════════════════════
//  PARTIELLES NEUZEICHNEN
// ═══════════════════════════════════════════════════════════════════

void clearRingZone() {
  // Nur Ring-Bereich (86-108 Radius) + äußerer Rand
  for (int y = 12; y < 228; y++) {
    for (int x = 12; x < 228; x++) {
      int dx = x - CX;
      int dy = y - CY;
      int r = (int)sqrt(dx * dx + dy * dy);
      if ((r >= 82 && r <= 112) || (r >= 82 && r <= 87)) {
        gfx->writePixel(x, y, BLACK);
      }
    }
  }
}

void clearScreenZones() {
  gfx->fillCircle(CX, CY, 81, BLACK);
  for (int r = 109; r <= 116; r++) gfx->drawCircle(CX, CY, r, BLACK);
  gfx->fillRect(40, 6, 160, 16, BLACK);
  gfx->fillRect(60, 208, 120, 18, BLACK);
}

// Setting-Name oben (nur bei Bedarf zeichnen)
void drawHeader() {
  if (!needHeaderRedraw && curSetting == prevSetting) return;
  gfx->fillRect(40, 6, 160, 16, BLACK);
  drawCentered(SETTING_NAMES[curSetting], 14, 1, 0x8410);
  prevSetting = curSetting;
}

// 5 Punkte unten — zeigt aktive Seite (optimiert)
void drawSideDots() {
  if (!needDotsRedraw && curSide == prevSide) return;
  
  for (int i = 0; i < NUM_SIDES; i++) {
    int x = CX - 28 + i * 14;
    gfx->fillCircle(x, 218, 4, BLACK);
    if (i == curSide) {
      gfx->fillCircle(x, 218, 4, hsv8To565(cfg[i].hue, cfg[i].sat, 220));
    } else {
      gfx->fillCircle(x, 218, 2, 0x2104);
    }
  }
  prevSide = curSide;
}

// Randring: nur bei Änderung zeichnen
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
//  SCREEN: HELLIGKEIT (OPTIMIERT)
// ═══════════════════════════════════════════════════════════════════
void drawBrightnessScreen() {
  if (needFullRedraw) {
    clearScreenZones();
  }

  uint8_t  bri   = cfg[curSide].bri;
  uint16_t sCol  = hsv8To565(cfg[curSide].hue, cfg[curSide].sat, 200);
  uint16_t empty = 0x18C3;

  // Nur Ring bei Änderung neuzeichnen
  if (needFullRedraw || needArcRedraw || 
      cfgPrev[curSide].bri != bri ||
      cfgPrev[curSide].hue != cfg[curSide].hue ||
      cfgPrev[curSide].sat != cfg[curSide].sat) {
    
    // Hintergrund-Ring (leer)
    fillArcSector(CX, CY, 86, 108, 225.0f, 495.0f, empty);

    // Befüllter Arc
    if (bri > 0) {
      fillArcSector(CX, CY, 86, 108, 225.0f, 225.0f + 270.0f * (bri / 255.0f), sCol);
    }

    // Endpunkt-Markierungen
    fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
    fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);
  }

  // Prozentwert
  gfx->fillRect(80, CY - 30, 80, 24, BLACK);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (bri * 100) / 255);
  drawCentered(buf, CY - 18, 3, WHITE);

  drawHeader();
  drawSideDots();
  drawEditBorder();
  
  cfgPrev[curSide] = cfg[curSide];
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: FARBE (HUE) - OPTIMIERT
// ═══════════════════════════════════════════════════════════════════
void drawHueScreen() {
  if (needFullRedraw) {
    clearScreenZones();
    
    // Regenbogenring (je 2°-Segment für bessere Performance)
    for (int deg = 0; deg < 360; deg += 2) {
      uint8_t  h8 = (uint8_t)((uint32_t)deg * 255 / 360);
      fillArcSector(CX, CY, 86, 108, (float)deg, (float)(deg + 2),
                    hsv8To565(h8, 255, 200));
    }
  }

  // Marker an aktuellem Farbwert
  if (needFullRedraw || needArcRedraw || cfgPrev[curSide].hue != cfg[curSide].hue) {
    float markerDeg = (float)cfg[curSide].hue * 360.0f / 255.0f;
    // Erase old marker
    gfx->fillCircle(CX, CY, 120, BLACK);
    // Draw new marker
    drawArcMarker(CX, CY, 97, markerDeg,
                  WHITE, hsv8To565(cfg[curSide].hue, 255, 230));
  }

  // Farbkreis Mitte
  gfx->fillCircle(CX, CY, 58,
                  hsv8To565(cfg[curSide].hue, cfg[curSide].sat, 200));

  drawHeader();
  drawSideDots();
  drawEditBorder();
  
  cfgPrev[curSide] = cfg[curSide];
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: SAETTIGUNG - OPTIMIERT
// ═══════════════════════════════════════════════════════════════════
void drawSaturationScreen() {
  if (needFullRedraw) {
    clearScreenZones();

    uint8_t hue = cfg[curSide].hue;

    // Gradient-Ring (30 Segmente statt 60)
    const int SEG = 30;
    for (int i = 0; i < SEG; i++) {
      uint8_t  s  = (uint8_t)((uint32_t)i * 255 / SEG);
      float    sA = 225.0f + (float)i * 270.0f / SEG;
      float    eA = 225.0f + (float)(i + 1) * 270.0f / SEG + 0.5f;
      fillArcSector(CX, CY, 86, 108, sA, eA, hsv8To565(hue, s, 200));
    }

    // Endpunkt-Markierungen
    fillArcSector(CX, CY, 82, 112, 222.0f, 228.0f, 0x4208);
    fillArcSector(CX, CY, 82, 112, 492.0f, 498.0f, 0x4208);
  }

  uint8_t hue = cfg[curSide].hue;
  uint8_t sat = cfg[curSide].sat;

  // Marker
  if (needFullRedraw || needArcRedraw || cfgPrev[curSide].sat != sat) {
    gfx->fillCircle(CX, CY, 120, BLACK);
    float markerDeg = 225.0f + (sat / 255.0f) * 270.0f;
    drawArcMarker(CX, CY, 97, markerDeg,
                  WHITE, hsv8To565(hue, sat, 230));
  }

  // Farbkreis Mitte
  gfx->fillCircle(CX, CY, 58, hsv8To565(hue, sat, 200));

  drawHeader();
  drawSideDots();
  drawEditBorder();
  
  cfgPrev[curSide] = cfg[curSide];
}

// ═══════════════════════════════════════════════════════════════════
//  SCREEN: SEITE - OPTIMIERT
// ═══════════════════════════════════════════════════════════════════
void drawSideScreen() {
  if (needFullRedraw) {
    clearScreenZones();
    gfx->fillCircle(CX, CY, 87, BLACK);

    const float GAP     = 5.0f;
    const float SWEEP   = (360.0f - NUM_SIDES * GAP) / NUM_SIDES;

    for (int i = 0; i < NUM_SIDES; i++) {
      float startDeg = -90.0f + i * (SWEEP + GAP);
      float endDeg   = startDeg + SWEEP;
      bool  active   = (i == curSide);

      uint16_t c = active
        ? hsv8To565(cfg[i].hue, cfg[i].sat, 220)
        : dim565(hsv8To565(cfg[i].hue, cfg[i].sat, 200), 0.2f);

      int outerR = active ? 108 : 103;
      int innerR = active ? 80  :  87;
      fillArcSector(CX, CY, innerR, outerR, startDeg, endDeg, c);

      // Seitennummer
      float midRad = (startDeg + SWEEP / 2.0f) * (float)M_PI / 180.0f;
      int   tx     = CX + (int)(62 * sinf(midRad)) - 4;
      int   ty     = CY - (int)(62 * cosf(midRad)) - 6;
      gfx->setTextSize(1);
      gfx->setTextColor(active ? WHITE : 0x4208);
      gfx->setCursor(tx, ty);
      gfx->print(i + 1);
    }
  }

  // Aktive Seitennummer in der Mitte
  gfx->fillRect(80, CY - 30, 80, 24, BLACK);
  char buf[3];
  snprintf(buf, sizeof(buf), "%d", curSide + 1);
  drawCentered(buf, CY - 18, 4, WHITE);

  drawHeader();
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
  
  needFullRedraw = false;
  needArcRedraw = false;
  needHeaderRedraw = false;
  needDotsRedraw = false;
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
  
  // Initialize previous config
  for (int i = 0; i < NUM_SIDES; i++) {
    cfgPrev[i] = cfg[i];
  }
}

// ═══════════════════════════════════════════════════════════════════
//  ENCODER-HANDLING (OPTIMIERT)
// ═══════════════════════════════════════════════════════════════════
void handleEncoder() {
  // Debouncing
  if (millis() - lastEncoderMs < ENCODER_DEBOUNCE_MS) return;
  
  long count = encoder.getCount() / 2;
  int  delta = (int)(count - lastCount);
  if (delta == 0) return;
  lastCount = count;
  lastEncoderMs = millis();

  if (!editMode) {
    // Browse: zwischen Settings wechseln
    curSetting = (curSetting + delta + NUM_SETTINGS) % NUM_SETTINGS;
    needFullRedraw = true;
  } else {
    // Edit: Wert ändern
    switch (curSetting) {
      case SETTING_BRI:
        cfg[curSide].bri = (uint8_t)constrain((int)cfg[curSide].bri + delta * 5, 0, 255);
        needArcRedraw = true;
        break;
      case SETTING_HUE:
        cfg[curSide].hue = (uint8_t)((cfg[curSide].hue + delta * 3 + 256) % 256);
        needArcRedraw = true;
        break;
      case SETTING_SAT:
        cfg[curSide].sat = (uint8_t)constrain((int)cfg[curSide].sat + delta * 5, 0, 255);
        needArcRedraw = true;
        break;
      case SETTING_SIDE:
        curSide = (curSide + delta + NUM_SIDES) % NUM_SIDES;
        needFullRedraw = true;
        break;
    }
    updateLeds();
    savePending = true;
    lastEditMs  = millis();
  }
}

void handleButton() {
  if (digitalRead(PIN_ENC_SW) != LOW) return;
  if (millis() - lastBtnMs < 250) return;
  lastBtnMs = millis();

  editMode   = !editMode;
  needBorderRedraw = true;
}

// ═══════════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Display
  gfx->begin();
  gfx->fillScreen(BLACK);

  // Encoder
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(PIN_ENC_CLK, PIN_ENC_DT);
  encoder.setCount(0);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  // LEDs
  leds.begin();
  leds.setBrightness(255);
  leds.show();

  // Trigonometrische Lookup-Tables initialisieren
  initTrigTables();

  loadConfig();
  updateLeds();
  needFullRedraw = true;
  
  lastFrameMs = millis();
}

void loop() {
  unsigned long now = millis();

  // Input-Handling (jederzeit möglich)
  handleButton();
  handleEncoder();

  // Frame-Rate-Limiting: nur alle 16ms updaten (60 FPS)
  if (now - lastFrameMs >= FRAME_TIME_MS) {
    lastFrameMs = now;
    
    if (needFullRedraw || needArcRedraw || needHeaderRedraw || 
        needDotsRedraw || needBorderRedraw) {
      drawScreen();
    }
  }

  // Verzögertes Speichern (3s nach letzter Änderung)
  if (savePending && millis() - lastEditMs > 3000) {
    saveConfig();
    savePending = false;
  }
}
