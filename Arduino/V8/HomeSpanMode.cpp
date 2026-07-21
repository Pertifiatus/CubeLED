#include <HomeSpan.h>
#include "CubeShared.h"
#include "HomeSpanMode.h"

// Setup-Code ohne Bindestriche (HomeSpan verlangt exakt 8 Ziffern),
// entspricht dem angezeigten Code 466-37-726.
static const char* PAIRING_CODE = "46637726";
static const char* QR_SETUP_ID  = "CLED";

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
    ledsShow();

    return true;
  }
};

void homeSpanModeSetup() {
  Serial.begin(115200);

  gfx->begin();
  gfx->fillScreen(BLACK);

  leds.begin();
  leds.setBrightness(255);
  leds.show();

  homeSpan.setPairingCode(PAIRING_CODE);
  homeSpan.setQRID(QR_SETUP_ID);
  homeSpan.begin(Category::Lighting, "CubeLED");

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CubeLED");
    new DEV_CubeLight();
}

void homeSpanModeLoop() {
  homeSpan.poll();
}
