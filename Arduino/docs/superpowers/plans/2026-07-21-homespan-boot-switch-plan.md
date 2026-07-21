# HomeSpan Boot-Switch (V8) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a boot-time hardware switch to the CubeLED ESP32-C3 sketch that
selects, per boot, whether the device runs the existing local cube UI or a
HomeSpan HomeKit accessory that controls the same 80 LEDs — all inside a new
`V8/` copy, leaving `V7/` byte-for-byte untouched.

**Architecture:** One firmware image. `setup()` reads GPIO20 once, caches the
decision, and dispatches to either `cubeSetup()`/`cubeLoop()` (the renamed,
unmodified V7 logic) or `homeSpanModeSetup()`/`homeSpanModeLoop()` (new,
in `V8/HomeSpanMode.cpp`). Shared hardware (LED strip, display) is exposed
across translation units via a small `V8/CubeShared.h` header of `extern`
declarations.

**Tech Stack:** Arduino/ESP32-C3, Arduino_GFX_Library, Adafruit NeoPixel,
Preferences, HomeSpan (Gregg Berman), QRCode (ricmoo). Verified against
`arduino-cli`.

## Global Constraints

- Board: ESP32-C3, FQBN `esp32:esp32:esp32c3:PartitionScheme=huge_app`
  (verify with `arduino-cli board listall esp32c3` if this FQBN doesn't
  resolve on the installed core version). The `huge_app` partition scheme
  (3MB app / 1MB SPIFFS, no second OTA slot) is **required** once HomeSpan
  is linked in — HomeSpan's WiFi/HAP stack alone overflows the default
  dual-OTA partitioning (~1.25MB per app slot) by ~279KB, discovered while
  compiling Task 3. OTA is not used anywhere in this project, so trading
  the second app slot for headroom is free. `V7/` and the plain-copy `V8/`
  from Tasks 1-2 already compiled fine under the plain `esp32:esp32:esp32c3`
  FQBN (no HomeSpan code yet) — this partition scheme only matters from
  Task 3 onward, once `HomeSpanMode.cpp` links against `<HomeSpan.h>`.
- `V7/V7.ino` must remain byte-for-byte unchanged. Every task that touches
  files must `git status`/`git diff` to confirm `V7/` shows no changes.
- No physical hardware is available this session. "Testing" = confirm each
  task compiles cleanly via `arduino-cli compile`. Real boot/pairing
  behavior must be verified by the user on the device.
- New pin: GPIO20 = `PIN_MODE_SWITCH`, `INPUT_PULLUP`, LOW = HomeSpan mode,
  HIGH = Cube mode. Read once in `setup()`, cached, never re-read in `loop()`.
- HomeKit pairing code is `466-37-726`, but HomeSpan's `setPairingCode()`
  requires exactly 8 digits with **no dashes**: pass `"46637726"`.
- QR Setup ID is a fixed 4-char literal `"CLED"` — must be passed identically
  to both `homeSpan.setQRID(...)` and our own `HapQR::get(...)` call, or the
  displayed QR code won't match the paired device.
- HomeSpan mode's LightBulb state is fully independent from the Cube mode's
  `Preferences("cube")` — no state sharing between modes.
- Display backlight has no software control (tied to 3.3V) — HomeSpan mode's
  "off" state is simply `gfx->fillScreen(BLACK)`.

---

### Task 1: Arduino toolchain setup + baseline compile of untouched V7

**Files:**
- Create: `tools/arduino-cli/` (downloaded binary + config, not committed —
  add to `.gitignore`)
- Create: `.gitignore` (new, or append if one already exists at repo root)

**Interfaces:**
- Produces: a working `arduino-cli` binary at
  `tools/arduino-cli/arduino-cli.exe`, ESP32 core `esp32:esp32` installed,
  libraries `Arduino_GFX_Library`, `Adafruit NeoPixel`, `HomeSpan`, `QRCode`
  installed. All later tasks' compile-check steps depend on this.

- [ ] **Step 1: Download and extract arduino-cli**

```powershell
New-Item -ItemType Directory -Force -Path "tools\arduino-cli" | Out-Null
Invoke-WebRequest -Uri "https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip" -OutFile "tools\arduino-cli\arduino-cli.zip"
Expand-Archive -Path "tools\arduino-cli\arduino-cli.zip" -DestinationPath "tools\arduino-cli" -Force
& "tools\arduino-cli\arduino-cli.exe" version
```

Expected: prints a version string like `arduino-cli  Version: 1.x.x ...`.

- [ ] **Step 2: Configure board index and install the ESP32 core**

```powershell
& "tools\arduino-cli\arduino-cli.exe" config init --overwrite --config-file "tools\arduino-cli\arduino-cli.yaml"
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" core update-index
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" core install esp32:esp32
```

Expected: ends with `Platform esp32:esp32@X.Y.Z installed`. This step
downloads ~1GB and may take several minutes — run with a long timeout.

- [ ] **Step 3: Install required libraries**

```powershell
$cli = "tools\arduino-cli\arduino-cli.exe"
& $cli --config-file "tools\arduino-cli\arduino-cli.yaml" lib install "GFX Library for Arduino"
& $cli --config-file "tools\arduino-cli\arduino-cli.yaml" lib install "Adafruit NeoPixel"
& $cli --config-file "tools\arduino-cli\arduino-cli.yaml" lib install "HomeSpan"
& $cli --config-file "tools\arduino-cli\arduino-cli.yaml" lib install "QRCode"
```

Note: the Library Manager name for `Arduino_GFX_Library` is
`"GFX Library for Arduino"` (moononournation) — confirm with
`arduino-cli lib search Arduino_GFX` if install fails on this exact string.

Expected: each command ends with `Successfully installed <name>@<version>`.

- [ ] **Step 4: Baseline-compile the untouched V7 sketch**

```powershell
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" compile --fqbn esp32:esp32:esp32c3 "V7"
```

Expected: ends with `Sketch uses ... bytes ...` and no `error:` lines. This
is the baseline every later task's compile check is compared against —
if V7 doesn't compile cleanly here, stop and fix the toolchain setup before
continuing (do not touch `V7/V7.ino` itself to "fix" it).

- [ ] **Step 5: Ignore the downloaded toolchain and commit**

Add to `.gitignore` (create the file at the repo root if it doesn't exist):

```
tools/arduino-cli/
```

```bash
git add .gitignore
git commit -m "Ignore local arduino-cli toolchain directory"
```

---

### Task 2: Copy V7 to V8 and verify identical compile

**Files:**
- Create: `V8/V8.ino` (copy of `V7/V7.ino`, renamed)
- Create: `V8/FreeSansBold18pt7b.h` or any other non-`.ino` file that exists
  in `V7/` (copy whatever `V7/` contains besides `V7.ino` — check with
  `Get-ChildItem V7` first; as of this plan `V7/` contains only `V7.ino`)

**Interfaces:**
- Produces: `V8/V8.ino` — a byte-identical copy of `V7/V7.ino` except for
  the file name. All later tasks modify this file.

- [ ] **Step 1: List V7's contents so nothing is missed in the copy**

```powershell
Get-ChildItem "V7"
```

- [ ] **Step 2: Copy the folder and rename the sketch file**

```powershell
Copy-Item -Recurse "V7" "V8"
Rename-Item "V8\V7.ino" "V8.ino"
```

- [ ] **Step 3: Confirm V7 is untouched**

```powershell
git status --porcelain -- V7
```

Expected: no output (empty) — `V7/` has zero changes.

- [ ] **Step 4: Compile V8 and confirm it matches the V7 baseline**

```powershell
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" compile --fqbn esp32:esp32:esp32c3 "V8"
```

Expected: same "Sketch uses ... bytes" success as Task 1 Step 4 (V8 is
currently identical code, just a different folder/file name).

- [ ] **Step 5: Commit**

```bash
git add V8
git commit -m "Copy V7 to V8 as the base for the HomeSpan boot-switch work"
```

---

### Task 3: Boot-mode switch, dispatcher, and a bare pairable HomeSpan accessory

**Files:**
- Create: `V8/CubeShared.h`
- Create: `V8/HomeSpanMode.h`
- Create: `V8/HomeSpanMode.cpp`
- Modify: `V8/V8.ino` (rename existing `setup()`/`loop()`, add new
  dispatcher `setup()`/`loop()`, add two `#include` lines)

**Interfaces:**
- Consumes (from `V8/V8.ino`, exposed via `CubeShared.h`):
  - `extern Adafruit_NeoPixel leds;`
  - `extern Arduino_GFX *gfx;`
  - `void setSideColor(int side, uint32_t color);`
  - `void ledsShow();`
  - `uint32_t hsv8ToNeo(uint8_t h, uint8_t s, uint8_t v);`
  - `#define PIN_MODE_SWITCH 20`
  - `#define PIN_ENC_SW 21`
  - `#define BLACK 0x0000`, `#define WHITE 0xFFFF`, `#define CX 120`,
    `#define CY 120`, `#define NUM_SIDES 5`
- Produces (for Task 4 and Task 5, declared in `V8/HomeSpanMode.h`):
  - `void homeSpanModeSetup();`
  - `void homeSpanModeLoop();`

- [ ] **Step 1: Create `V8/CubeShared.h`**

```cpp
#pragma once

#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>

// Geteilt zwischen dem Cube-Modus (V8.ino) und dem HomeSpan-Modus
// (HomeSpanMode.cpp) -- Boot-Wahlschalter und Encoder-Taster.
#define PIN_MODE_SWITCH 20   // LOW = HomeSpan-Modus, HIGH (Pullup) = Cube-Modus
#define PIN_ENC_SW      21

#define BLACK  0x0000
#define WHITE  0xFFFF
#define CX     120
#define CY     120
#define NUM_SIDES  5

extern Adafruit_NeoPixel leds;
extern Arduino_GFX      *gfx;

void     setSideColor(int side, uint32_t color);
void     ledsShow();
uint32_t hsv8ToNeo(uint8_t h, uint8_t s, uint8_t v);
```

- [ ] **Step 2: Create `V8/HomeSpanMode.h`**

```cpp
#pragma once

void homeSpanModeSetup();
void homeSpanModeLoop();
```

- [ ] **Step 3: Create `V8/HomeSpanMode.cpp` (bare pairable accessory, no light control yet)**

```cpp
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
```

- [ ] **Step 4: Edit `V8/V8.ino` — add includes right after the existing ones**

Find:
```cpp
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
```

Replace with:
```cpp
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "CubeShared.h"
#include "HomeSpanMode.h"
```

- [ ] **Step 5: Edit `V8/V8.ino` — rename the existing setup/loop**

Find:
```cpp
void setup() {
  Serial.begin(115200);

  initTrigTables();
```

Replace with:
```cpp
void cubeSetup() {
  Serial.begin(115200);

  initTrigTables();
```

Find:
```cpp
void loop() {
  unsigned long now = millis();

  handleButton();
  handleEncoder();
```

Replace with:
```cpp
void cubeLoop() {
  unsigned long now = millis();

  handleButton();
  handleEncoder();
```

- [ ] **Step 6: Edit `V8/V8.ino` — append the new dispatcher at the end of the file**

Append after the final closing brace of the (now renamed) `cubeLoop()`:

```cpp

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
```

- [ ] **Step 7: Confirm V7 is still untouched**

```powershell
git status --porcelain -- V7
```

Expected: no output.

- [ ] **Step 8: Compile V8**

```powershell
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" "V8"
```

Expected: `Sketch uses ... bytes ...`, no `error:` lines. This confirms the
dispatcher, the shared header's `extern` declarations, and the bare
HomeSpan accessory all link correctly.

- [ ] **Step 9: Commit**

```bash
git add V8
git commit -m "Add GPIO20 boot-mode switch and a bare pairable HomeSpan accessory to V8"
```

---

### Task 4: HomeKit LightBulb service driving the physical LEDs

**Files:**
- Modify: `V8/HomeSpanMode.cpp`

**Interfaces:**
- Consumes: `setSideColor()`, `ledsShow()`, `hsv8ToNeo()`, `leds`,
  `NUM_SIDES` (all from `CubeShared.h`, already included).
- Produces: `struct DEV_CubeLight` (file-local to `HomeSpanMode.cpp`, no
  other task needs to reference it directly).

- [ ] **Step 1: Add the `DEV_CubeLight` service struct above `homeSpanModeSetup()`**

Find:
```cpp
static const char* PAIRING_CODE = "46637726";
static const char* QR_SETUP_ID  = "CLED";

void homeSpanModeSetup() {
```

Replace with:
```cpp
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
```

- [ ] **Step 2: Instantiate the service in `homeSpanModeSetup()`**

Find:
```cpp
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CubeLED");
}
```

Replace with:
```cpp
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("CubeLED");
    new DEV_CubeLight();
}
```

- [ ] **Step 3: Confirm V7 is still untouched**

```powershell
git status --porcelain -- V7
```

Expected: no output.

- [ ] **Step 4: Compile V8**

```powershell
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" "V8"
```

Expected: `Sketch uses ... bytes ...`, no `error:` lines.

- [ ] **Step 5: Commit**

```bash
git add V8/HomeSpanMode.cpp
git commit -m "Drive the LED cube from a HomeKit LightBulb service in HomeSpan mode"
```

---

### Task 5: QR-code pairing display on encoder-button click

**Files:**
- Create: `V8/QRCodeRicmoo.h` (vendored copy of the installed `QRCode`
  library's `src/qrcode.h`, renamed)
- Create: `V8/QRCodeRicmoo.c` (vendored copy of the installed `QRCode`
  library's `src/qrcode.c`, renamed, with its internal include updated)
- Modify: `V8/HomeSpanMode.cpp`

**Interfaces:**
- Consumes: `PIN_ENC_SW`, `gfx`, `BLACK`, `WHITE`, `CX`, `CY` (from
  `CubeShared.h`, already included); `Category::Lighting` and `HapQR` (from
  `<HomeSpan.h>`, already included); `qrcode_getBufferSize`,
  `qrcode_initText`, `qrcode_getModule`, `QRCode`, `ECC_LOW` (from
  `"QRCodeRicmoo.h"`, vendored this task — see note below).
- Produces: nothing consumed by later tasks — this is the last task.

**Why a vendored copy instead of `#include <qrcode.h>`:** the ESP32-C3
Arduino core bundles its own internal component (`espressif__qrcode`,
namespaced under `esp_qrcode_*` — no symbol overlap with the installed
library) whose own header is also named `qrcode.h`. The core's build system
always resolves `#include <qrcode.h>` to its own header first, regardless of
library install order, silently shadowing the `QRCode` library installed in
Task 1. This is a known, previously-reported conflict
([ricmoo/QRCode#35](https://github.com/ricmoo/QRCode/issues/35)); the
community's accepted fix — confirmed here — is to rename the library's own
two files so the filename no longer collides. Since the renamed file lives
in `V8/` and is included with quotes (`"QRCodeRicmoo.h"`, not
`<qrcode.h>`), the sketch directory's own copy always resolves first,
regardless of the platform header of the same original name. The vendored
files' MIT license header (Richard Moore / Project Nayuki) must be kept
intact at the top of both copied files — do not strip it.

- [ ] **Step 1: Locate the installed QRCode library and vendor it under a new name**

Find where `arduino-cli` installed the `QRCode` library (Task 1 ran
`arduino-cli lib install "QRCode"`):

```powershell
$cli = "tools\arduino-cli\arduino-cli.exe"
$cfg = "tools\arduino-cli\arduino-cli.yaml"
& $cli --config-file $cfg config dump | Select-String "user:"
```

This prints the `directories.user` (sketchbook) path — the library sits at
`<that path>\libraries\QRCode\src\qrcode.h` and `qrcode.c`. Copy both into
`V8/`, renamed:

```powershell
$libSrc = "<directories.user path from above>\libraries\QRCode\src"
Copy-Item "$libSrc\qrcode.h" "V8\QRCodeRicmoo.h"
Copy-Item "$libSrc\qrcode.c" "V8\QRCodeRicmoo.c"
```

- [ ] **Step 2: Update the vendored `.c` file's internal include**

Open `V8/QRCodeRicmoo.c` and find the line (near the top, after the license
comment block):

```c
#include "qrcode.h"
```

Replace with:

```c
#include "QRCodeRicmoo.h"
```

This is the only content change needed in either vendored file — everything
else (including the MIT license header at the top of both files) stays
exactly as copied.

- [ ] **Step 3: Add the QR include and a numeric pairing-code constant**

Find:
```cpp
#include <HomeSpan.h>
#include "CubeShared.h"
#include "HomeSpanMode.h"

// Setup-Code ohne Bindestriche (HomeSpan verlangt exakt 8 Ziffern),
// entspricht dem angezeigten Code 466-37-726.
static const char* PAIRING_CODE = "46637726";
static const char* QR_SETUP_ID  = "CLED";
```

Replace with:
```cpp
#include <HomeSpan.h>
#include "QRCodeRicmoo.h"
#include "CubeShared.h"
#include "HomeSpanMode.h"

// Setup-Code ohne Bindestriche (HomeSpan verlangt exakt 8 Ziffern),
// entspricht dem angezeigten Code 466-37-726.
static const char*   PAIRING_CODE     = "46637726";
static const uint32_t PAIRING_CODE_NUM = 46637726;
static const char*   QR_SETUP_ID      = "CLED";
```

- [ ] **Step 4: Add the QR-rendering and button-toggle logic above `homeSpanModeSetup()`**

Find:
```cpp
void homeSpanModeSetup() {
```

Replace with:
```cpp
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
```

- [ ] **Step 5: Configure the encoder-button pin in `homeSpanModeSetup()`**

Find:
```cpp
  leds.begin();
  leds.setBrightness(255);
  leds.show();

  homeSpan.setPairingCode(PAIRING_CODE);
```

Replace with:
```cpp
  leds.begin();
  leds.setBrightness(255);
  leds.show();

  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  homeSpan.setPairingCode(PAIRING_CODE);
```

- [ ] **Step 6: Poll the button in `homeSpanModeLoop()`**

Find:
```cpp
void homeSpanModeLoop() {
  homeSpan.poll();
}
```

Replace with:
```cpp
void homeSpanModeLoop() {
  homeSpan.poll();
  handleQrToggleButton();
}
```

- [ ] **Step 7: Confirm V7 is still untouched**

```powershell
git status --porcelain -- V7
```

Expected: no output.

- [ ] **Step 8: Compile V8**

```powershell
& "tools\arduino-cli\arduino-cli.exe" --config-file "tools\arduino-cli\arduino-cli.yaml" compile --fqbn "esp32:esp32:esp32c3:PartitionScheme=huge_app" "V8"
```

Expected: `Sketch uses ... bytes ...`, no `error:` lines.

- [ ] **Step 9: Commit**

```bash
git add V8/HomeSpanMode.cpp V8/QRCodeRicmoo.h V8/QRCodeRicmoo.c
git commit -m "Show the HomeKit pairing QR code on encoder-button click in HomeSpan mode"
```

---

## Post-Plan Notes (for the user, not a task)

- First boot in HomeSpan mode with no WiFi configured yet: `homeSpanModeSetup()`
  calls `homeSpan.enableAutoStartAP()` (added post-plan, commit `551cf4d`),
  so HomeSpan automatically opens its own temporary setup access point named
  `CubeLED-Setup` — connect a phone or computer to it, fill in your home
  WiFi credentials in the page it serves, and the device joins your network
  and becomes pairable on the next boot. No serial monitor or button needed;
  this only fires when no WiFi credentials are stored yet (e.g. first boot
  after a resale), never on ordinary subsequent boots. The Serial CLI's `W`
  command remains available as a manual alternative if a computer with a
  serial connection happens to be at hand.
- The QR `SCALE` constant in `drawPairingQR()` is a calibration knob — check
  on the real round display and adjust if the code is clipped by the bezel
  or too small for a phone camera to read reliably.
- Physical switch wiring: GPIO20 to GND selects HomeSpan mode; leaving it
  open (internal pull-up) selects the existing Cube mode. Reset/power-cycle
  after moving the switch.
- Discovered on real hardware (commit `d752a5e`): `homeSpan.poll()`'s very
  first call blocks for up to 300 seconds when no WiFi credentials are
  stored yet — the auto-start Access Point (`enableAutoStartAP()`) runs
  *synchronously inside* that first `poll()` call, not in the background.
  Confirmed via Serial Monitor: after "Starting Access Point... Ready." the
  log goes silent until either WiFi is entered via the captive portal or
  the AP times out and the device reboots. This means the onboarding
  screen (`updateOnboardScreen()`, driven from `homeSpanModeLoop()`) cannot
  run — and cannot react to the encoder — for the entire duration of that
  first blocking call. The WiFi step screen is now painted once directly
  in `homeSpanModeSetup()`, before the first `loop()`/`poll()` call, so the
  display shows the join-WiFi instructions throughout this window instead
  of staying black. Scrolling to the QR step only becomes possible once
  WiFi is actually joined (i.e. once that first blocking `poll()` call
  finally returns).
- If the onboarding screen or scrolling ever seems unresponsive again on
  real hardware, check the Serial Monitor first (115200 baud, "Newline"
  line ending): lowercase `s` prints current pairing/connection status,
  `F` performs a factory reset (clears stored WiFi + pairing data) for a
  clean retest.
