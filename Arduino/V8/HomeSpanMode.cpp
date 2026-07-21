#include <HomeSpan.h>
#include "CubeShared.h"
#include "HomeSpanMode.h"

// Setup-Code ohne Bindestriche (HomeSpan verlangt exakt 8 Ziffern),
// entspricht dem angezeigten Code 466-37-726.
static const char* PAIRING_CODE = "46637726";
static const char* QR_SETUP_ID  = "CLED";

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
}

void homeSpanModeLoop() {
  homeSpan.poll();
}
