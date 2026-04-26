#include <U8g2lib.h>
#include <ESP32Encoder.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <Wire.h>

// --- Hardware Pins ---
#define PIN_OLED_SDA 8
#define PIN_OLED_SCL 9
#define PIN_NEOPIXEL 10

// Encoder & Software-Power
#define PIN_ENC_S1  3   // Entspricht CLK / A
#define PIN_ENC_S2  4   // Entspricht DT / B
#define PIN_ENC_KEY 2   // Entspricht SW / Button
#define PIN_ENC_GND 5   // Software GND
#define PIN_ENC_VCC 6   // Software VCC (3.3V)

// --- LED Setup ---
#define NUM_STRIPS 4
#define LEDS_PER_STRIP 8
#define TOTAL_LEDS (NUM_STRIPS * LEDS_PER_STRIP)

Adafruit_NeoPixel strip(TOTAL_LEDS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, PIN_OLED_SCL, PIN_OLED_SDA);
ESP32Encoder encoder;
Preferences prefs;

// --- Datenstrukturen ---
struct StripConfig {
  uint8_t hue;
  uint8_t sat;
  uint8_t bri;
};

StripConfig strips[NUM_STRIPS];
uint8_t masterBrightness = 150;
int currentEffect = 0; 

// --- Menü Zustände ---
enum MenuState { MAIN_MENU, SUB_MENU, EDIT_VAL };
MenuState menuState = MAIN_MENU;

int mainPos = 0;   
int subPos = 0;    
int editVal = 0;   

const int MAIN_ITEMS_COUNT = 7;
const char* mainItems[] = {
  "Master-Helligkeit", "Effekt waehlen", 
  "Strip 1 Config", "Strip 2 Config", 
  "Strip 3 Config", "Strip 4 Config", 
  "Alle Strips Config"
};

const int SUB_ITEMS_COUNT = 3;
const char* subItems[] = { "Hue (Farbe)", "Saturation (Saettigung)", "Brightness (Helligkeit)" };
const char* effectNames[] = { "Static", "Soft Ember", "Vert Flow" };

unsigned long lastButtonPress = 0;
unsigned long lastLedUpdate = 0;
unsigned long lastEditTime = 0;
bool savePending = false;
const unsigned long SAVE_DELAY = 3000; 

void setup() {
  Serial.begin(115200);

  // --- Software Power für Encoder ---
  pinMode(PIN_ENC_GND, OUTPUT);
  digitalWrite(PIN_ENC_GND, LOW);   // GND Level
  pinMode(PIN_ENC_VCC, OUTPUT);
  digitalWrite(PIN_ENC_VCC, HIGH);  // 3.3V Level

  // Taster mit internem Pullup
  pinMode(PIN_ENC_KEY, INPUT_PULLUP);

  // OLED
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  display.begin();
  display.setContrast(200);

  // Encoder initialisieren
  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(PIN_ENC_S1, PIN_ENC_S2);
  encoder.setCount(0);

  // LEDs
  strip.begin();
  strip.show();

  loadConfig();
}

void loop() {
  handleEncoder();
  
  if (millis() - lastLedUpdate > 30) {
    updateLeds();
    lastLedUpdate = millis();
  }

  if (savePending && (millis() - lastEditTime > SAVE_DELAY)) {
    saveConfig();
    savePending = false;
  }
}

void handleEncoder() {
  static long lastCount = 0;
  long currentCount = encoder.getCount() / 2;
  int delta = currentCount - lastCount;

  // --- Button Klick (KEY) ---
  if (digitalRead(PIN_ENC_KEY) == LOW) {
    if (millis() - lastButtonPress > 250) { 
      lastButtonPress = millis();
      
      if (menuState == MAIN_MENU) {
        if (mainPos == 0) { menuState = EDIT_VAL; editVal = masterBrightness; }
        else if (mainPos == 1) { menuState = EDIT_VAL; editVal = currentEffect; }
        else { menuState = SUB_MENU; subPos = 0; } 
      } 
      else if (menuState == SUB_MENU) {
        menuState = EDIT_VAL;
        int sIdx = mainPos - 2;
        if (sIdx == 4) sIdx = 0; 
        if (subPos == 0) editVal = strips[sIdx].hue;
        if (subPos == 1) editVal = strips[sIdx].sat;
        if (subPos == 2) editVal = strips[sIdx].bri;
      } 
      else if (menuState == EDIT_VAL) {
        menuState = (mainPos <= 1) ? MAIN_MENU : SUB_MENU;
        savePending = true;
        lastEditTime = millis();
      }
      drawDisplay();
    }
  }

  // --- Drehung (S1/S2) ---
  if (delta != 0) {
    if (menuState == MAIN_MENU) {
      mainPos = constrain(mainPos + delta, 0, MAIN_ITEMS_COUNT - 1);
    } 
    else if (menuState == SUB_MENU) {
      subPos = constrain(subPos + delta, 0, SUB_ITEMS_COUNT - 1);
    } 
    else if (menuState == EDIT_VAL) {
      if (mainPos == 0) {
        editVal = constrain(editVal + delta * 5, 0, 255);
        masterBrightness = editVal;
      } else if (mainPos == 1) {
        editVal = constrain(editVal + delta, 0, 2);
        currentEffect = editVal;
      } else {
        if (subPos == 0) editVal = (editVal + delta * 5 + 256) % 256; 
        else editVal = constrain(editVal + delta * 5, 0, 255);        
        
        // Werte direkt anwenden
        int sIdx = mainPos - 2;
        if (sIdx == 4) { // Alle Strips
          for(int i=0; i<4; i++) {
            if (subPos == 0) strips[i].hue = editVal;
            if (subPos == 1) strips[i].sat = editVal;
            if (subPos == 2) strips[i].bri = editVal;
          }
        } else { // Einzeln
          if (subPos == 0) strips[sIdx].hue = editVal;
          if (subPos == 1) strips[sIdx].sat = editVal;
          if (subPos == 2) strips[sIdx].bri = editVal;
        }
      }
      lastEditTime = millis();
      savePending = true;
    }
    lastCount = currentCount;
    drawDisplay();
  }
}

void drawDisplay() {
  display.clearBuffer();
  display.setFont(u8g2_font_5x7_tr);

  if (menuState == MAIN_MENU) {
    display.drawStr(0, 8, "--- HAUPTMENUE ---");
    for (int i = 0; i < 4; i++) {
      int itemIdx = mainPos - 1 + i;
      if (itemIdx >= 0 && itemIdx < MAIN_ITEMS_COUNT) {
        int y = 25 + (i * 12);
        if (itemIdx == mainPos) display.drawStr(0, y, ">");
        display.drawStr(10, y, mainItems[itemIdx]);
      }
    }
  } 
  else if (menuState == SUB_MENU) {
    display.drawStr(0, 8, mainItems[mainPos]);
    for (int i = 0; i < SUB_ITEMS_COUNT; i++) {
      int y = 25 + (i * 12);
      if (i == subPos) display.drawStr(0, y, ">");
      display.drawStr(10, y, subItems[i]);
    }
  } 
  else if (menuState == EDIT_VAL) {
    display.setFont(u8g2_font_6x10_tf);
    if (mainPos == 0) display.drawStr(0, 15, "Edit: Master-Hell.");
    else if (mainPos == 1) display.drawStr(0, 15, "Edit: Effekt");
    else display.drawStr(0, 15, subItems[subPos]);

    if (mainPos == 1) {
      display.setFont(u8g2_font_7x13B_tr);
      display.drawStr(20, 40, effectNames[editVal]);
    } else {
      char buf[16];
      sprintf(buf, "Wert: %d", editVal);
      display.drawStr(30, 35, buf);
      display.drawFrame(10, 45, 108, 10);
      int barWidth = (editVal * 104) / 255;
      display.drawBox(12, 47, barWidth, 6);
    }
  }
  display.sendBuffer();
}

void updateLeds() {
  uint32_t t = millis();
  for (int s = 0; s < NUM_STRIPS; s++) {
    for (int l = 0; l < LEDS_PER_STRIP; l++) {
      int pixelIdx = (s * LEDS_PER_STRIP) + l;
      uint32_t color = 0;
      uint8_t finalBri = (strips[s].bri * masterBrightness) / 255;

      if (currentEffect == 0) {
        color = strip.ColorHSV(strips[s].hue * 256, strips[s].sat, finalBri);
      } 
      else if (currentEffect == 1) {
        float noise = random(80, 100) / 100.0f; 
        float fade = 1.0f - (l / (float)LEDS_PER_STRIP);
        color = strip.ColorHSV(strips[s].hue * 256, strips[s].sat, (uint8_t)(finalBri * fade * noise));
      } 
      else if (currentEffect == 2) {
        uint16_t flowHue = (t / 5) + (l * 2000); 
        color = strip.ColorHSV(flowHue, 255, finalBri); 
      }
      strip.setPixelColor(pixelIdx, strip.gamma32(color));
    }
  }
  strip.show();
}

void saveConfig() {
  prefs.begin("lamp", false);
  prefs.putUChar("masterBri", masterBrightness);
  prefs.putInt("effect", currentEffect);
  for (int i = 0; i < NUM_STRIPS; i++) {
    char key[10];
    sprintf(key, "h%d", i); prefs.putUChar(key, strips[i].hue);
    sprintf(key, "s%d", i); prefs.putUChar(key, strips[i].sat);
    sprintf(key, "b%d", i); prefs.putUChar(key, strips[i].bri);
  }
  prefs.end();
}

void loadConfig() {
  prefs.begin("lamp", true);
  masterBrightness = prefs.getUChar("masterBri", 150);
  currentEffect = prefs.getInt("effect", 0);
  for (int i = 0; i < NUM_STRIPS; i++) {
    char key[10];
    sprintf(key, "h%d", i); strips[i].hue = prefs.getUChar(key, 0);
    sprintf(key, "s%d", i); strips[i].sat = prefs.getUChar(key, 255);
    sprintf(key, "b%d", i); strips[i].bri = prefs.getUChar(key, 255);
  }
  prefs.end();
  drawDisplay();
}