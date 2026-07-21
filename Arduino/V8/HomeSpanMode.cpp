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
//  QR-CODE PAIRING-ANZEIGE -- Klick auf den Encoder-Taster zeigt/versteckt
//  den HomeKit-Pairing-QR-Code auf dem sonst schwarzen Display.
// ═══════════════════════════════════════════════════════════════════
#define BTN_DEBOUNCE_MS 30
static bool          btnRawLast  = HIGH;
static bool          btnStable   = HIGH;
static unsigned long btnChangeMs = 0;
static bool          qrVisible   = false;

static void drawPairingQR() {
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
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        gfx->fillRect(offset + x * SCALE, offset + y * SCALE, SCALE, SCALE, BLACK);
      }
    }
  }
}

static void handleQrToggleButton() {
  bool raw = digitalRead(PIN_ENC_SW);

  if (raw != btnRawLast) {
    btnChangeMs = millis();
    btnRawLast  = raw;
  }
  if (millis() - btnChangeMs < BTN_DEBOUNCE_MS) return;
  if (raw == btnStable) return;
  btnStable = raw;
  if (btnStable != LOW) return;

  qrVisible = !qrVisible;
  if (qrVisible) drawPairingQR();
  else           gfx->fillScreen(BLACK);
}

void homeSpanModeSetup() {
  Serial.begin(115200);

  gfx->begin();
  gfx->fillScreen(BLACK);

  leds.begin();
  leds.setBrightness(255);
  leds.show();

  pinMode(PIN_ENC_SW, INPUT_PULLUP);

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
}

void homeSpanModeLoop() {
  homeSpan.poll();
  handleQrToggleButton();
}
