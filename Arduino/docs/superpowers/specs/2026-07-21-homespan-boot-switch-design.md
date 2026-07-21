# Boot-Schalter: Cube-Modus vs. HomeSpan/HomeKit-Modus (V8)

## Kontext

`V7/V7.ino` (ESP32-C3, GC9A01-Rundlisplay per SPI, 80x WS2812B, Rotary Encoder)
ist der aktuelle, funktionierende Stand und bleibt **unverändert** als Backup.
Alle Änderungen dieser Spec landen in einem neuen, aus V7 kopierten Ordner
`V8/V8.ino` (+ neue Zusatzdateien).

Ziel: ein physischer Schalter erlaubt es, vor dem Einschalten festzulegen, ob
das Gerät in den bisherigen lokalen Cube-Modus bootet (Encoder + TFT steuern
Farbe/Helligkeit/Effekte direkt) oder in einen HomeSpan-Modus, der den Würfel
als HomeKit-Lampe über WLAN steuerbar macht.

## Hardware

- Neuer Schalter an **GPIO20**, `INPUT_PULLUP`. Fest verdrahtete Zwei-Stellungs-
  Position (kein Taster): Pin gegen GND = HomeSpan-Modus, offen/HIGH = Cube-
  Modus.
- GPIO20 ist auf dem ESP32-C3 SuperMini frei, kein Strapping-Pin, keine
  Boot-Sonderrolle.
- Alle übrigen Pins (TFT: 4,5,6,7,10 · NeoPixel: 0 · Encoder: 3,1,21) bleiben
  wie in V7 unverändert.
- Display-Backlight liegt aktuell fest an 3.3V (kein Software-Ein/Aus möglich).
  Freier Pin für spätere Nachrüstung: GPIO8 (nicht Teil dieser Spec, nur
  Hinweis für die Zukunft — nicht implementieren, da nicht verkabelt).

## Boot-Verzweigung

- `setup()` liest GPIO20 **einmalig**, ganz am Anfang, vor jeder anderen
  Initialisierung.
- Je nach Zustand läuft entweder die bestehende Cube-Logik (umbenannt in
  `cubeSetup()` / `cubeLoop()`, inhaltlich 1:1 aus V7 übernommen) oder die
  neue HomeSpan-Logik (`homeSpanModeSetup()` / `homeSpanModeLoop()`).
- Umschalten erfordert einen Reset/Neustart mit umgelegtem Schalter — kein
  Live-Wechsel zur Laufzeit.

## HomeSpan-Modus

- Meldet ein HomeKit-Accessory mit `Service::LightBulb` an, Characteristics
  `On`, `Brightness`, `Hue`, `Saturation`.
- Characteristic-Handler rechnen HSV auf dieselbe Farbdarstellung wie im
  Cube-Modus um (Re-Use von `hsv8ToNeo()`, `setSideColor()`, `ledsShow()`)
  und schreiben sie auf alle 80 LEDs.
- **Eigener, von Cube-Preferences unabhängiger Zustand.** Kein Teilen von
  gespeicherter Farbe/Helligkeit zwischen den Modi — HomeSpan persistiert
  Characteristic-Werte ohnehin selbst in NVS.
- Pairing-Code: `466-37-726`, gesetzt über `homeSpan.setPairingCode(...)`.
- TFT bleibt im HomeSpan-Modus standardmäßig **schwarz**.
- Encoder-Rotation: in diesem Modus ungenutzt.
- Encoder-Taster-Klick: zeigt den HomeKit-Pairing-QR-Code (die von HomeSpan
  bereitgestellte `X-HM://...`-Setup-URI) auf dem Rund-Display an. Encoding
  über die schlanke Header-only-Library `ricmoo/QRCode` (keine weiteren
  Abhängigkeiten) — von Hand nachzubauen wäre unverhältnismäßig für diesen
  Zweck.
- Erneuter Klick: zurück zu schwarzem Bildschirm (einfacher Toggle).

## Dateistruktur

- `V7/V7.ino` bleibt exakt wie es ist (Backup / Timeline-Anker).
- Neuer Ordner `V8/`, 1:1-Kopie von `V7/`, umbenannt zu `V8/V8.ino`.
- In `V8/V8.ino`: bestehende `setup()`/`loop()` werden zu `cubeSetup()`/
  `cubeLoop()`, neue schlanke `setup()`/`loop()` an der Spitze der Datei
  übernehmen nur die Boot-Verzweigung.
- Neue Dateien `V8/HomeSpanMode.h` + `V8/HomeSpanMode.cpp`: HomeSpan-Setup,
  LightBulb-Callbacks, QR-Anzeige-Logik. Hält V8.ino nicht weiter aufgebläht
  und trennt die beiden Modi klar voneinander.

## Out of Scope

- Kein Live-Umschalten zwischen Modi ohne Reset.
- Kein Software-Backlight (Pin nicht verkabelt).
- Kein geteilter Zustand zwischen Cube- und HomeSpan-Modus.
- Keine Änderungen an `V7/V7.ino`.

## Testing

- Kein physischer Hardwarezugriff in dieser Session möglich. Verifikation
  beschränkt sich auf Kompilierbarkeit (Arduino-CLI, falls verfügbar),
  Pin-Konflikt-Prüfung und Abgleich der HomeSpan-API-Aufrufe gegen die
  Library-Dokumentation. Echter Boot-/Pairing-Test am Gerät liegt beim
  Nutzer.
