# pico-parkhaus

Parking garage with two Raspberry Pi Pico W.

- **Pico 1 (Kasse):** opens the Wi-Fi, calculates the fee, payment at Kasse or Automat, exit barrier
- **Pico 2 (Schranken):** camera simulation (enter plate), entry barrier, asks the Kasse if a car paid

## Setup

1. Flash `kasse` on Pico 1 and `schranken` on Pico 2 (PlatformIO):
   ```
   pio run -e kasse -t upload
   pio run -e schranken -t upload
   ```
   If upload fails: hold BOOTSEL, plug in, copy `.pio/build/<env>/firmware.uf2` to the `RPI-RP2` drive.
2. Connect to Wi-Fi `Parkhaus-Gruppe1` (password `parkhaus123`).
3. Open http://192.168.42.1/ (Kasse). It links to the Schranken page.

Settings are in `include/config.h`.

## Wiring

| Barrier | Pico | Signal |
| --- | --- | --- |
| Einfahrt (left) | Schranken | GP8 (pin 11) |
| Ausfahrt (right) | Kasse | GP8 (pin 11) |

SG90 servos: brown = GND, red = 5V (VBUS), orange = signal.

## Rules

- Plate: canton + 1-6 digits, e.g. `ZH 123456`
- First 15 min free, then CHF 2.00 per started hour, max CHF 20.00 per day
- After paying you have 15 min to leave
- 50 spaces
- Barriers close after 5 s
- Data is lost when the Kasse restarts
