#include <HomeSpan.h>
#include "QRCodeRicmoo.h"
#include "CubeShared.h"
#include "HomeSpanMode.h"

// Setup-Code ohne Bindestriche (HomeSpan verlangt exakt 8 Ziffern),
// entspricht dem angezeigten Code 466-37-726.
static const char*   PAIRING_CODE     = "46637726";
static const uint32_t PAIRING_CODE_NUM = 46637726;
static const char*   QR_SETUP_ID      = "CLED";

// HomeKit-Lampe -- steuert dieselben 80 LEDs wie der Cube-Modus, aber mit
// eigenem, von den Cube-Preferences unabhaengigem Zustand.
struct DEV_CubeLight : Service::LightBulb {
  SpanCharacteristic *power;
  SpanCharacteristic *H;
  SpanCharacteristic *S;
  SpanCharacteristic *V;

  DEV_CubeLight() : Service::LightBulb() {
    power = new Characteristic::On();
    H     = new Characteristic::Hue(0);
    S     = new Characteristic::Saturation(0);
    V     = new Characteristic::Brightness(100);
    V->setRange(0, 100, 1);
  }

  boolean update() {
    float h = H->getVal<float>();
    float s = S->getVal<float>();
    float v = V->getVal<float>();
    bool  p = power->getVal();

    if (power->updated()) p = power->getNewVal();
    if (H->updated())     h = H->getNewVal<float>();
    if (S->updated())     s = S->getNewVal<float>();
    if (V->updated())     v = V->getNewVal<float>();

    uint8_t h8 = (uint8_t)(h / 360.0f * 255.0f);
    uint8_t s8 = (uint8_t)(s / 100.0f * 255.0f);
    uint8_t v8 = p ? (uint8_t)(v / 100.0f * 255.0f) : 0;

    uint32_t c = leds.gamma32(hsv8ToNeo(h8, s8, v8));  // gleiche Gamma-Korrektur wie im Cube-Modus
    for (int side = 0; side < NUM_SIDES; side++) setSideColor(side, c);
    leds.show();  // no cube-mode encoder ISR running in HomeSpan mode, so no need for ledsShow()'s ISR guard

    return true;
  }
};

// ═══════════════════════════════════════════════════════════════════
//  ONBOARDING-ANZEIGE -- solange das Geraet noch nicht mit HomeKit
//  gekoppelt ist, zeigt das Display zwei per Drehgeber umschaltbare
//  Schritte (WLAN verbinden / QR-Code scannen). Nach dem Pairing bleibt
//  der Bildschirm schwarz, wie bisher.
// ═══════════════════════════════════════════════════════════════════
enum OnboardPage { PAGE_WIFI = 0, PAGE_QR = 1 };

// ponytail: einfache Flankenerkennung nur auf PIN_ENC_CLK, Drehrichtung
// wird ignoriert -- bei nur 2 Seiten reicht "irgendeine Rastung = umschalten"
// vollkommen; volle Gray-Code-Quadraturdekodierung (wie im Cube-Modus) waere
// hier unnoetiger Aufwand. Bei mehr als 2 Seiten neu bewerten.
#define ENC_TICK_DEBOUNCE_MS 150
static bool          encClkLast    = HIGH;
static unsigned long encTickMs     = 0;
static int           onboardPage   = PAGE_WIFI;
static bool          wasPaired     = false;
static int           lastDrawnPage = -1;

static bool isPaired() {
  return homeSpan.controllerListBegin() != homeSpan.controllerListEnd();
}

static void drawWifiStepScreen() {
  gfx->fillScreen(BLACK);
  drawCentered("APPLE HOME",                   30, 2, WHITE);
  drawCentered("SCROLLEN: NAECHSTER SCHRITT",   56, 1, 0x8410);

  drawCentered("SCHRITT 1",                    100, 1, WHITE);
  drawCentered("WLAN VERBINDEN:",               124, 1, WHITE);
  drawCentered("\"CubeLED-Setup\"",             140, 1, WHITE);
  drawCentered("WLAN-DATEN EINGEBEN",           164, 1, WHITE);
}

static void drawQrStepScreen() {
  HapQR qrEncoder;
  char *uri = qrEncoder.get(PAIRING_CODE_NUM, QR_SETUP_ID, (uint8_t)Category::Lighting);

  const uint8_t QR_VERSION = 3;  // 29x29 Module, reicht fuer den ~20 Zeichen langen URI
  uint8_t qrData[qrcode_getBufferSize(QR_VERSION)];
  QRCode qrcode;
  qrcode_initText(&qrcode, qrData, QR_VERSION, ECC_LOW, uri);

  // ponytail: SCALE ist ein Kalibrierknopf -- am echten Rund-Display pruefen
  // und anpassen, damit der QR-Code weder zu klein zum Scannen noch an den
  // Ecken vom runden Gehaeuse abgeschnitten wird.
  const int SCALE  = 4;
  int       offset = CX - (qrcode.size * SCALE) / 2;

  gfx->fillScreen(WHITE);
  drawCentered("SCHRITT 2",          32, 1, BLACK);
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        gfx->fillRect(offset + x * SCALE, offset + y * SCALE, SCALE, SCALE, BLACK);
      }
    }
  }
  drawCentered("ZURUECK: SCROLLEN", 190, 1, BLACK);
}

static void handleOnboardScroll() {
  bool clk = digitalRead(PIN_ENC_CLK);
  if (clk == LOW && encClkLast == HIGH && millis() - encTickMs > ENC_TICK_DEBOUNCE_MS) {
    onboardPage = (onboardPage == PAGE_WIFI) ? PAGE_QR : PAGE_WIFI;
    encTickMs   = millis();
  }
  encClkLast = clk;
}

// Zeigt den Onboarding-Flow, solange das Geraet nicht gekoppelt ist; sobald
// isPaired() feststellt, dass ein Controller gepairt wurde, wird der
// Bildschirm einmalig geschwaerzt und bleibt es (kein Re-Onboarding noetig).
static void updateOnboardScreen() {
  bool paired = isPaired();

  if (paired) {
    if (!wasPaired) { gfx->fillScreen(BLACK); lastDrawnPage = -1; }
    wasPaired = true;
    return;
  }

  if (wasPaired) lastDrawnPage = -1;  // frisch entpaart -> Onboarding sofort neu zeichnen
  wasPaired = false;

  handleOnboardScroll();

  if (onboardPage != lastDrawnPage) {
    if (onboardPage == PAGE_WIFI) drawWifiStepScreen();
    else                          drawQrStepScreen();
    lastDrawnPage = onboardPage;
  }
}

void homeSpanModeSetup() {
  Serial.begin(115200);

  gfx->begin();
  gfx->fillScreen(BLACK);

  leds.begin();
  leds.setBrightness(255);
  leds.show();

  pinMode(PIN_ENC_CLK, INPUT_PULLUP);

  homeSpan.setPairingCode(PAIRING_CODE);
  homeSpan.setQRID(QR_SETUP_ID);
  // Kein WLAN gespeichert (z.B. Erstinbetriebnahme nach Verkauf) -> HomeSpan
  // startet automatisch einen eigenen Setup-Access-Point mit Webformular,
  // kein serieller Monitor noetig. Danach normaler Boot ins Heimnetz.
  homeSpan.setApSSID("CubeLED-Setup");
  homeSpan.enableAutoStartAP();
  homeSpan.begin(Category::Lighting, "CubeLED");

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CubeLED");
    new DEV_CubeLight();

  // homeSpan.poll()'s allererster Aufruf blockiert, falls noch kein WLAN
  // gespeichert ist, bis zu 300s lang (der Auto-Start-Access-Point laeuft
  // synchron *innerhalb* dieses ersten Aufrufs) -- deshalb hier schon vor
  // loop() den ersten Onboarding-Screen zeichnen, sonst bleibt das Display
  // waehrend der ganzen WLAN-Einrichtung schwarz und reagiert auf nichts.
  drawWifiStepScreen();
  lastDrawnPage = PAGE_WIFI;
}

void homeSpanModeLoop() {
  homeSpan.poll();
  updateOnboardScreen();
}
