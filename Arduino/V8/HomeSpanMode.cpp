#include <HomeSpan.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
//  gekoppelt ist, zeigt das Display drei per Drehgeber umschaltbare
//  Seiten (Titel / WLAN verbinden / QR-Code scannen). Nach dem Pairing
//  bleibt der Bildschirm schwarz, wie bisher.
// ═══════════════════════════════════════════════════════════════════
enum OnboardPage { PAGE_TITLE = 0, PAGE_WIFI = 1, PAGE_QR = 2, NUM_ONBOARD_PAGES = 3 };

// ponytail: einfache Flankenerkennung nur auf PIN_ENC_CLK, Drehrichtung
// wird ignoriert -- eine Rastung schaltet immer zur naechsten Seite (zyklisch).
// Volle Gray-Code-Quadraturdekodierung (wie im Cube-Modus) waere hier
// unnoetiger Aufwand fuer ein simples Vor-Karussell durch 3 Infoseiten.
#define ENC_TICK_DEBOUNCE_MS 150
static bool          encClkLast    = HIGH;
static unsigned long encTickMs     = 0;
static int           onboardPage   = PAGE_TITLE;
static bool          wasPaired     = false;
static int           lastDrawnPage = -1;

// Mit autoPoll() laeuft HomeSpan auf einem eigenen Task -- das Lesen seiner
// Controller-Liste von hier aus muesste daher eigentlich ueber HomeSpans
// Shared-Mutex abgesichert werden (homeSpanPAUSE/RESUME). PER HARDWARE-TEST
// BESTAETIGT: genau dieser Mutex wird vom Poll-Task waehrend seines allerersten
// pollTask()-Aufrufs als EXCLUSIVE Lock ueber die komplette Access-Point-Phase
// gehalten (bis zu 300s) -- ein homeSpanPAUSE-Versuch von hier aus wuerde also
// genauso lange blockieren wie die AP-Phase selbst. Ohne WLAN-Verbindung kann
// ohnehin kein Pairing existieren, daher wird der Mutex hier gar nicht erst
// angefasst, solange WiFi.status() nicht WL_CONNECTED ist -- das umgeht die
// Blockade vollstaendig, ohne die eigentliche Kernaussage (nicht gepairt) zu
// verfaelschen.
static bool isPaired() {
  if (WiFi.status() != WL_CONNECTED) return false;

  homeSpanPAUSE
  bool result = homeSpan.controllerListBegin() != homeSpan.controllerListEnd();
  homeSpanRESUME
  return result;
}

static void drawTitleScreen() {
  gfx->fillScreen(BLACK);
  drawCentered("APPLE HOME",       100, 2, WHITE);
  drawCentered("EINRICHTEN",       126, 2, WHITE);
  drawCentered("SCROLLEN: WEITER", 170, 1, 0x8410);
}

static void drawWifiStepScreen() {
  gfx->fillScreen(BLACK);
  drawCentered("SCHRITT 1",                 26, 2, WHITE);
  drawCentered("SCROLLEN: WEITER",          50, 1, 0x8410);

  drawCentered("WLAN VERBINDEN:",           88, 1, WHITE);
  drawCentered("\"CubeLED-Setup\"",        105, 1, WHITE);
  drawCentered("PASSWORT:",                127, 1, WHITE);
  drawCentered("\"homespan\"",             144, 1, WHITE);  // HomeSpan-Standardpasswort, nie ueberschrieben
  drawCentered("DANN WLAN-DATEN EINGEBEN", 168, 1, WHITE);
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
  drawCentered("SCHRITT 2", 32, 1, BLACK);
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        gfx->fillRect(offset + x * SCALE, offset + y * SCALE, SCALE, SCALE, BLACK);
      }
    }
  }
  drawCentered("SCROLLEN: WEITER", 190, 1, BLACK);
}

static void handleOnboardScroll() {
  bool clk = digitalRead(PIN_ENC_CLK);

  if (clk == LOW && encClkLast == HIGH && millis() - encTickMs > ENC_TICK_DEBOUNCE_MS) {
    onboardPage = (onboardPage + 1) % NUM_ONBOARD_PAGES;
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
    switch (onboardPage) {
      case PAGE_TITLE: drawTitleScreen();    break;
      case PAGE_WIFI:  drawWifiStepScreen(); break;
      case PAGE_QR:    drawQrStepScreen();   break;
    }
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

  // homeSpan.poll() blockiert bis zu 300s, falls noch kein WLAN gespeichert
  // ist (der Auto-Start-Access-Point laeuft synchron *innerhalb* des Aufrufs,
  // haelt dabei HomeSpans eigenen Mutex exklusiv). autoPoll() laesst HomeSpan
  // stattdessen auf einem eigenen FreeRTOS-Task laufen. WICHTIG:
  // homeSpan.poll() darf dann in homeSpanModeLoop() NICHT mehr manuell
  // aufgerufen werden (HomeSpan bricht sonst mit einem Fatal Error ab).
  homeSpan.autoPoll();

  // Mit der isPaired()-Loesung oben laeuft der Onboarding-Task jetzt auch
  // waehrend der AP-Phase normal weiter, daher kann hier gleich die
  // Titelseite gezeigt werden (Schritt 1 folgt nach dem ersten Scrollen).
  drawTitleScreen();
  onboardPage   = PAGE_TITLE;
  lastDrawnPage = PAGE_TITLE;

  // Eigener, hoeher priorisierter Task fuers Onboarding-Display/Encoder --
  // siehe isPaired() fuer den eigentlichen Grund, warum das vorher trotzdem
  // nicht reichte (Mutex-Blockade, nicht CPU-Prioritaet).
  xTaskCreate([](void*) {
    for (;;) {
      updateOnboardScreen();
      vTaskDelay(pdMS_TO_TICKS(30));
    }
  }, "onboardTask", 4096, NULL, 2, NULL);
}

void homeSpanModeLoop() {
  // Onboarding-Update laeuft in einem eigenen, hoeher priorisierten Task
  // (siehe homeSpanModeSetup) -- loop() bleibt hier bewusst leer.
}
