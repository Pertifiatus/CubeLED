/*
 * SmartDial – ESP32 Lamp Controller
 * Hardware: ESP32 + GC9A01 240x240 + WS2812B + Rotary Encoder
 * Libs: Arduino_GFX_Library, FastLED
 */

#include <Arduino_GFX_Library.h>
#include <FastLED.h>
#include <math.h>

// ── Pins ────────────────────────────────────────────────────────────────────
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

// ── Display ──────────────────────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_HWSPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI);
Arduino_GFX    *gfx = new Arduino_GC9A01(bus, PIN_TFT_RST, 0, true);

#define CX  120
#define CY  120

// ── LEDs ─────────────────────────────────────────────────────────────────────
CRGB leds[NUM_LEDS];

// ── Parameter ────────────────────────────────────────────────────────────────
struct Param {
  int       val;
  const int lo, hi, step;
  const bool wraps;
  const char *label;
};

// Index: 0=Helligkeit, 1=Farbe(Hue), 2=Saettigung, 3=Effekt
Param params[] = {
  {  80,   0, 100, 1, false, "HELLIGKEIT" },
  { 128,   0, 255, 3, true,  "FARBE"      },
  { 220,   0, 255, 3, false, "SAETTIGUNG" },
  {   0,   0,   3, 1, true,  "EFFEKT"     },
};
const int P_BRI = 0, P_HUE = 1, P_SAT = 2, P_EFF = 3, P_CNT = 4;
const char *effNames[] = { "STATIC", "PULS", "FLOW", "RAINBOW" };

// ── UI-State ─────────────────────────────────────────────────────────────────
enum State : uint8_t { DETAIL, ZOOM_OUT, MENU, ZOOM_IN };
State state     = DETAIL;
int   active    = P_BRI;   // aktuell bearbeiteter Parameter
int   cursor    = P_BRI;   // Menü-Auswahl
bool  needDraw  = true;

// Zoom-Animation
float         scale     = 1.0f;
float         menuAlpha = 0.0f;   // 0=Detail, 1=Menü
unsigned long animT0    = 0;
const float   SCALE_MIN = 0.49f;
const float   MENU_DY   = 22.0f;  // Dial-Shift nach oben im Menü
const int     ANIM_MS   = 300;

// ── Encoder ISR ──────────────────────────────────────────────────────────────
portMUX_TYPE    mux   = portMUX_INITIALIZER_UNLOCKED;
volatile int    encD  = 0;
volatile byte   lastAB = 0;

void IRAM_ATTR encISR() {
  byte a = digitalRead(PIN_ENC_A), b = digitalRead(PIN_ENC_B);
  byte ab = (a << 1) | b, sm = (lastAB << 2) | ab;
  if (sm==0b1101||sm==0b0100||sm==0b0010||sm==0b1011) { portENTER_CRITICAL_ISR(&mux); encD++; portEXIT_CRITICAL_ISR(&mux); }
  if (sm==0b1110||sm==0b0111||sm==0b0001||sm==0b1000) { portENTER_CRITICAL_ISR(&mux); encD--; portEXIT_CRITICAL_ISR(&mux); }
  lastAB = ab;
}

// ── Button (fallende Flanke, 15ms Debounce) ───────────────────────────────────
struct Button {
  int last=HIGH, stable=HIGH;
  bool clicked=false;
  unsigned long t=0;
} btn;

void pollBtn(unsigned long now) {
  int r = digitalRead(PIN_ENC_BTN);
  if (r != btn.last) { btn.t = now; btn.last = r; }
  if (now - btn.t > 15 && r != btn.stable) {
    btn.stable = r;
    if (r == LOW) btn.clicked = true;
  }
}

// ── Farb-Helfer ───────────────────────────────────────────────────────────────
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r&0xF8)<<8)|((uint16_t)(g&0xFC)<<3)|(b>>3);
}
uint32_t lerpRGB(uint32_t a, uint32_t b, float t) {
  uint8_t ar=a>>16,ag=a>>8,ab_=a, br=b>>16,bg=b>>8,bb_=b;
  return ((uint32_t)(ar+t*(br-ar))<<16)|((uint32_t)(ag+t*(bg-ag))<<8)|(uint8_t)(ab_+t*(bb_-ab_));
}
uint16_t to565(uint32_t c)        { return rgb565(c>>16,c>>8,c); }
uint32_t dim(uint32_t c, float f) { return (uint32_t)((c>>16&0xFF)*f)<<16|(uint32_t)((c>>8&0xFF)*f)<<8|(uint32_t)((c&0xFF)*f); }
uint16_t hsvTo565(uint8_t h, uint8_t s, uint8_t v) { CRGB c; hsv2rgb_rainbow(CHSV(h,s,v),c); return rgb565(c.r,c.g,c.b); }
float ease(float p) { return p<0.5f?2*p*p:1-powf(2-2*p,2)/2; }

// Text zentriert ausgeben
void textC(const char *s, int x, int y, uint8_t sz, uint16_t col) {
  gfx->setTextSize(sz); gfx->setTextColor(col, 0x0000);
  gfx->setTextWrap(false);
  gfx->setCursor(x - strlen(s)*3*sz, y - 4*sz);
  gfx->print(s);
}

// ── Arc ───────────────────────────────────────────────────────────────────────
// Arc-Geometrie: 135° (links-unten) → 405° (rechts-unten) = 270° Sweep
// 0° = 3-Uhr, Uhrzeigersinn
#define A_START 135.0f
#define A_END   405.0f
#define A_SPAN  270.0f
#define A_OUT   107.0f   // äußerer Radius (bei scale=1)
#define A_IN     88.0f   // innerer Radius

void arcLine(float cx, float cy, float ro, float ri, float deg, uint16_t col) {
  float r = deg * DEG_TO_RAD, c = cosf(r), s = sinf(r);
  gfx->drawLine(cx+c*ri, cy+s*ri, cx+c*ro, cy+s*ro, col);
}

void arcGrad(float cx, float cy, float ro, float ri, float a0, float a1, uint32_t ca, uint32_t cb) {
  if (a1 <= a0) return;
  for (float a = a0; a <= a1; a += 0.4f)
    arcLine(cx, cy, ro, ri, a, to565(lerpRGB(ca, cb, (a-a0)/(a1-a0))));
}

void arcSolid(float cx, float cy, float ro, float ri, float a0, float a1, uint16_t col) {
  for (float a = a0; a <= a1; a += 0.4f)
    arcLine(cx, cy, ro, ri, a, col);
}

// Weißer Dot auf dem Arc
void arcDot(float cx, float cy, float ro, float ri, float deg, float sc) {
  float r = deg*DEG_TO_RAD, mid = (ro+ri)*0.5f;
  int x = cx+cosf(r)*mid, y = cy+sinf(r)*mid, dr = max(2,(int)(5*sc));
  gfx->fillCircle(x, y, dr+2, 0x0000);
  gfx->fillCircle(x, y, dr,   0xFFFF);
}

// ── Arc je Parameter ─────────────────────────────────────────────────────────
void drawArcBri(float cx, float cy, float ro, float ri, float sc) {
  float t = params[P_BRI].val / 100.f;
  float ind = A_START + t * A_SPAN;
  uint32_t cEnd = lerpRGB(0x383838, 0xFFFFFF, t);
  arcSolid(cx, cy, ro, ri, A_START, A_END, to565(0x141414));
  if (t > 0.01f) {
    arcGrad(cx, cy, ro+4*sc, ri-4*sc, A_START, ind, dim(0x383838,.2f), dim(cEnd,.2f));
    arcGrad(cx, cy, ro, ri, A_START, ind, 0x383838, cEnd);
  }
  arcDot(cx, cy, ro, ri, ind, sc);
}

void drawArcHue(float cx, float cy, float ro, float ri, float sc) {
  // Dim-Rainbow als Track
  for (float a = A_START; a <= A_END; a += 0.4f)
    arcLine(cx, cy, ro, ri, a, hsvTo565((uint8_t)((a-A_START)/A_SPAN*255), 255, 65));
  // Glow am Indikator
  float t = params[P_HUE].val / 255.f;
  float ind = A_START + t * A_SPAN;
  for (float a = ind-10; a <= ind+10; a += 0.4f) {
    if (a < A_START || a > A_END) continue;
    float dist = fabsf(a-ind)/10.f;
    arcLine(cx, cy, ro+(1-dist)*4*sc, ri-(1-dist)*4*sc, a,
            hsvTo565((uint8_t)((a-A_START)/A_SPAN*255), 255, (uint8_t)(190*(1-dist*dist))));
  }
  arcDot(cx, cy, ro, ri, ind, sc);
}

void drawArcSat(float cx, float cy, float ro, float ri, float sc) {
  uint8_t h = params[P_HUE].val;
  float   t = params[P_SAT].val / 255.f;
  float ind = A_START + t * A_SPAN;
  CRGB viv; hsv2rgb_rainbow(CHSV(h,255,200), viv);
  uint32_t vCol = (uint32_t)viv.r<<16|(uint32_t)viv.g<<8|viv.b;
  uint32_t cEnd = lerpRGB(0x484848, vCol, t);
  arcSolid(cx, cy, ro, ri, A_START, A_END, to565(0x141414));
  if (t > 0.01f) {
    arcGrad(cx, cy, ro+4*sc, ri-4*sc, A_START, ind, dim(0x484848,.2f), dim(cEnd,.2f));
    arcGrad(cx, cy, ro, ri, A_START, ind, 0x484848, vCol);
  }
  arcDot(cx, cy, ro, ri, ind, sc);
}

void drawArcEff(float cx, float cy, float ro, float ri, float sc) {
  const uint32_t cols[4] = { 0xEEEEEE, 0x2255FF, 0x22DD77, 0xFF5511 };
  int sel = params[P_EFF].val;
  float zw = (A_SPAN - 6.f) / 4.f;   // Zone-Breite (66°), 1.5° Lücke
  for (int i = 0; i < 4; i++) {
    float a0 = A_START + i*(zw+1.5f), a1 = a0+zw;
    arcSolid(cx, cy, ro, ri, a0, a1, to565(i==sel ? cols[i] : dim(cols[i],.12f)));
    if (i == sel)
      arcSolid(cx, cy, ro+4*sc, ri-4*sc, a0, a1, to565(dim(cols[i],.22f)));
  }
  float ind = A_START + sel*(zw+1.5f) + zw*0.5f;
  arcDot(cx, cy, ro, ri, ind, sc);
}

// ── Dial-Mitte ────────────────────────────────────────────────────────────────
void drawCenter(int p, float sc, float cx, float cy) {
  if (sc < 0.55f) return;
  uint8_t h = params[P_HUE].val, s = params[P_SAT].val;
  uint8_t bv = map(params[P_BRI].val, 0, 100, 0, 220);

  if (p == P_BRI) {
    char buf[6]; sprintf(buf, "%d%%", params[P_BRI].val);
    textC(buf, cx, cy-5*sc, sc>=0.9f?6:5, 0xFFFF);
    textC("HELLIGKEIT", cx, cy+38*sc, 1, rgb565(85,85,85));

  } else if (p == P_HUE || p == P_SAT) {
    // Farbvorschau-Kreis
    int sr = 30*sc;
    gfx->fillCircle(cx, cy-6*sc, sr,   hsvTo565(h, s, max(150,bv)));
    gfx->drawCircle(cx, cy-6*sc, sr,   rgb565(45,45,45));
    textC(p==P_HUE?"FARBE":"SAETTIGUNG", cx, cy+38*sc, 1, rgb565(85,85,85));

  } else {  // P_EFF
    textC(effNames[params[P_EFF].val], cx, cy-5*sc, sc>=0.9f?3:2, 0xFFFF);
    textC("EFFEKT", cx, cy+38*sc, 1, rgb565(85,85,85));
  }
}

// ── Dial gesamt ───────────────────────────────────────────────────────────────
void drawDial(int p, float sc, float cx, float cy) {
  float ro = A_OUT*sc, ri = A_IN*sc;
  switch (p) {
    case P_BRI: drawArcBri(cx,cy,ro,ri,sc); break;
    case P_HUE: drawArcHue(cx,cy,ro,ri,sc); break;
    case P_SAT: drawArcSat(cx,cy,ro,ri,sc); break;
    case P_EFF: drawArcEff(cx,cy,ro,ri,sc); break;
  }
  drawCenter(p, sc, cx, cy);
}

// ── Menü-Dots + Label ────────────────────────────────────────────────────────
void drawMenu() {
  if (menuAlpha < 0.04f) return;
  const int dy = CY+74, gap = 20, x0 = CX-(P_CNT-1)*gap/2;
  for (int i = 0; i < P_CNT; i++) {
    int x = x0+i*gap;
    gfx->fillCircle(x, dy, 7, 0x0000);
    if (i==cursor) gfx->fillCircle(x, dy, 5, 0xFFFF);
    else           gfx->fillCircle(x, dy, 3, rgb565(55,55,55));
  }
  gfx->fillRect(CX-80, dy+8, 160, 12, 0x0000);
  textC(params[cursor].label, CX, dy+15, 1, 0xFFFF);
}

// ── Frame ────────────────────────────────────────────────────────────────────
void renderFrame() {
  bool anim  = (state==ZOOM_OUT || state==ZOOM_IN);
  float dialCY = CY - MENU_DY*menuAlpha;
  int   displayP = (state==DETAIL && menuAlpha<0.05f) ? active : cursor;

  if (anim) {
    // Voller Clear bei Zoom (Radius ändert sich)
    gfx->fillScreen(0x0000);
    gfx->drawCircle(CX,CY,118, rgb565(38,38,38));
    gfx->drawCircle(CX,CY,119, rgb565(22,22,22));
  } else {
    // Nur Dial-Area löschen (Außenring bleibt)
    // clearR = A_OUT*scale+9 < 118 (Außenring-Radius) immer ✓
    gfx->fillCircle(CX, dialCY, A_OUT*scale+9, 0x0000);
  }

  drawDial(displayP, scale, CX, dialCY);
  drawMenu();
}

// ── LEDs ─────────────────────────────────────────────────────────────────────
static uint8_t effHue = 0;
static float   breath = 0;

void updateLEDs() {
  uint8_t bri = map(params[P_BRI].val, 0, 100, 0, 255);
  uint8_t hue = params[P_HUE].val;
  uint8_t sat = params[P_SAT].val;
  switch (params[P_EFF].val) {
    case 0: fill_solid(leds, NUM_LEDS, CHSV(hue,sat,bri)); FastLED.setBrightness(255); break;
    case 1: breath+=0.06f; fill_solid(leds,NUM_LEDS,CHSV(hue,sat,(uint8_t)(bri*(0.25f+0.75f*(sinf(breath)*0.5f+0.5f))))); FastLED.setBrightness(255); break;
    case 2: effHue++; for(int i=0;i<NUM_LEDS;i++) leds[i]=CHSV(hue+effHue+i*4,sat,bri); FastLED.setBrightness(255); break;
    case 3: effHue++; fill_rainbow(leds,NUM_LEDS,effHue,256/NUM_LEDS); FastLED.setBrightness(bri); break;
  }
  FastLED.show();
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  gfx->begin(80000000);          // 80 MHz SPI – bei Problemen auf 40000000 reduzieren
  gfx->fillScreen(0x0000);
  gfx->drawCircle(CX,CY,118, rgb565(38,38,38));
  gfx->drawCircle(CX,CY,119, rgb565(22,22,22));

  pinMode(PIN_ENC_A,   INPUT_PULLUP);
  pinMode(PIN_ENC_B,   INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  FastLED.addLeds<WS2812B, PIN_LED, GRB>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 3000);
  updateLEDs();
  renderFrame();
}

// ── Loop ─────────────────────────────────────────────────────────────────────
unsigned long lastEff = 0;

void loop() {
  unsigned long now = millis();

  // Button
  pollBtn(now);
  if (btn.clicked) {
    btn.clicked = false;
    if      (state==DETAIL) { cursor=active; state=ZOOM_OUT; animT0=now; }
    else if (state==MENU)   { active=cursor; state=ZOOM_IN;  animT0=now; }
    needDraw = true;
  }

  // Encoder
  int d = 0;
  portENTER_CRITICAL(&mux); d=encD; encD=0; portEXIT_CRITICAL(&mux);
  if (d) {
    if (state==DETAIL) {
      Param &p = params[active];
      if (p.wraps) {
        int r = p.hi-p.lo+1;
        p.val = ((p.val-p.lo+d*p.step)%r+r)%r+p.lo;
      } else {
        p.val = constrain(p.val+d*p.step, p.lo, p.hi);
      }
      updateLEDs();
    } else if (state==MENU) {
      cursor = (cursor+d+P_CNT)%P_CNT;
    }
    needDraw = true;
  }

  // Animation
  if (state==ZOOM_OUT || state==ZOOM_IN) {
    float prog = constrain((float)(now-animT0)/ANIM_MS, 0.f, 1.f);
    float e = ease(prog);
    if (state==ZOOM_OUT) { scale=1.f-(1.f-SCALE_MIN)*e; menuAlpha=e;     if(prog>=1.f) state=MENU; }
    else                 { scale=SCALE_MIN+(1.f-SCALE_MIN)*e; menuAlpha=1.f-e; if(prog>=1.f){state=DETAIL;scale=1.f;menuAlpha=0.f;} }
    needDraw = true;
  }

  // Effekt-LED-Timer
  if (params[P_EFF].val > 0 && now-lastEff >= 30) { lastEff=now; updateLEDs(); }

  // Render
  if (needDraw) { needDraw=false; renderFrame(); }
}
