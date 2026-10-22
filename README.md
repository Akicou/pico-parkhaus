# pico-parkhaus

Parkhaus mit Ein- und Ausfahrtsschranke auf zwei Raspberry Pi Pico W, umgesetzt mit Arduino.

## Gruppenmitglieder

- Philipp
- Lyan

## Umsetzung

- **Pico 1 (Kasse):** Weboberfläche Kasse und Automat. Speichert die Parkvorgänge, berechnet die Gebühr, nimmt Zahlungen an. Hat eine eigene Uhr, die man in der Kasse stellen kann (Testfunktion *Uhrzeit setzen*).
- **Pico 2 (Schranken):** Weboberfläche Kamera-Simulation (Kennzeichen eintippen). Steuert beide Schranken (Servos) und fragt bei der Ausfahrt die Kasse, ob bezahlt wurde.

Beide Picos hängen im gleichen Hotspot. Pico 2 findet die Kasse über `parkhaus-kasse.local`. Die Daten liegen nur im RAM, nach einem Neustart sind sie weg.

Tarif: CHF 2.00 pro angefangene Stunde, mindestens eine Stunde.

## Umgesetzte User Stories

| ID | User Story | Umsetzung | Testfälle |
| --- | --- | --- | --- |
| US-01 | Kennzeichen erkennen | Eingabe in der Kamera-Simulation, Kasse speichert Kennzeichen und Einfahrtszeit | T01–T04 |
| US-02 | Schranke bei der Einfahrt | öffnet bei gültigem Kennzeichen, schliesst nach 5 s, bleibt bei leerer/ungültiger Eingabe zu | T01–T03, T05 |
| US-03 | Parkgebühr | Kasse und Automat zeigen Einfahrt, Parkdauer und Betrag | T06–T08 |
| US-04 | Bezahlen | Bezahlen an Kasse oder Automat, Fahrzeug wird als bezahlt markiert, keine doppelte Zahlung | T09–T11 |
| US-05 | Ausfahrt | Pico 2 fragt die Kasse, Schranke öffnet nur wenn bezahlt, bleibt zu wenn Kasse nicht antwortet | T12–T14, T18 |
| US-06 | Ausfahrt speichern | Ausfahrtszeit wird gespeichert, Auto wechselt zu den abgeschlossenen Parkvorgängen | T15 |
| US-07 | Fehler bei der Erkennung | Fehlermeldung mit Knopf *Hilfe anfordern*, Anfrage erscheint in der Kasse mit Ort und Uhrzeit | T16 |
| US-08 | Parkvorgänge anzeigen | Kasse zeigt aktuelle und abgeschlossene Parkvorgänge | T17 |

## Aufbau

Beide Servos hängen an Pico 2:

| Servo | Signal (orange) | Strom (rot) | GND (braun) |
| --- | --- | --- | --- |
| Einfahrt (links) | GP8, Pin 11 | VBUS, Pin 40 | GND, Pin 8 |
| Ausfahrt (rechts) | GP9, Pin 12 | VBUS, Pin 40 | GND, Pin 13 |

## Starten

1. Hotspot-Name und Passwort in `include/config.h` eintragen.
2. Mit PlatformIO hochladen:
   ```
   pio run -e kasse -t upload
   pio run -e schranken -t upload
   ```
   Falls der Upload nicht geht: BOOTSEL gedrückt halten, einstecken und `.pio/build/<env>/firmware.uf2` auf das Laufwerk `RPI-RP2` kopieren.
3. Laptop mit dem gleichen Hotspot verbinden und http://parkhaus-kasse.local/ öffnen. Die Kasse verlinkt die Schranken.
   Klappt `.local` nicht: Die IP-Adressen stehen im Serial Monitor (115200 Baud).
