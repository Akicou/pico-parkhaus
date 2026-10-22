// Pico 1 (Kasse)
//  - Opens the Wi-Fi network WIFI_SSID at 192.168.42.1
//  - Web page: list of parked cars, fee calculation, payment at Kasse or Automat
//  - API used by Pico 2 (Schranken):
//      POST /api/entry?plate=ZH123   -> 200 OK | 400 INVALID | 409 INSIDE | 507 FULL
//      GET  /api/status?plate=ZH123  -> 200 PAID | UNPAID <rappen> | 404 UNKNOWN
//      POST /api/exit?plate=ZH123    -> 200 PAID (car removed) | 402 UNPAID <rappen> | 404 UNKNOWN
//      GET  /api/hello               -> 200 OK, remembers the Schranken address for the page link
//
// Hardware: SG90 servo for the exit barrier (Ausfahrt, right) on GP8 (pin 11). It opens when
// /api/exit approves a car.
// The onboard LED is on while the access point is running.

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "barrier.h"
#include "config.h"

struct Car {
    String plate;
    uint32_t entryMs = 0;
    bool paid = false;
    uint32_t paidMs = 0;
    uint32_t paidRappen = 0;
    String paidAt;  // "Kasse" or "Automat"
};

struct Payment {
    String plate;
    uint32_t minutes;
    uint32_t rappen;
    String method;
    uint32_t atMs;
};

Car cars[CAPACITY];
size_t carCount = 0;

constexpr size_t LOG_SIZE = 20;
Payment payments[LOG_SIZE];
size_t paymentCount = 0;  // total ever, newest at (paymentCount - 1) % LOG_SIZE

WebServer server(80);
Barrier exitBarrier{"Ausfahrt", BARRIER_SERVO_PIN, EXIT_OPEN_US, EXIT_CLOSED_US};
IPAddress schrankenIp;  // learned from /api/hello

// ---------------------------------------------------------------------------
// Tariff
// ---------------------------------------------------------------------------

uint32_t feeForMinutes(uint32_t minutes) {
    if (minutes <= FREE_MINUTES) {
        return 0;
    }
    const uint32_t days = minutes / 1440;
    const uint32_t rest = minutes % 1440;
    const uint32_t restHours = (rest + 59) / 60;  // started hours
    const uint32_t restFee = min(restHours * RATE_PER_HOUR_RAPPEN, DAY_MAX_RAPPEN);
    return days * DAY_MAX_RAPPEN + restFee;
}

// What the car still owes right now. 0 while inside the exit grace period after paying.
uint32_t amountDue(const Car& car) {
    const uint32_t total = feeForMinutes(minutesSince(car.entryMs));
    if (car.paid) {
        if (minutesSince(car.paidMs) <= EXIT_GRACE_MINUTES) {
            return 0;
        }
        return total > car.paidRappen ? total - car.paidRappen : 0;
    }
    return total;
}

Car* findCar(const String& plate) {
    for (size_t i = 0; i < carCount; ++i) {
        if (cars[i].plate == plate) {
            return &cars[i];
        }
    }
    return nullptr;
}

void removeCar(Car* car) {
    const size_t i = car - cars;
    cars[i] = cars[carCount - 1];
    cars[carCount - 1] = Car{};
    --carCount;
}

void pay(Car& car, const String& method) {
    const uint32_t due = amountDue(car);
    car.paid = true;
    car.paidMs = millis();
    car.paidRappen += due;
    car.paidAt = method;

    payments[paymentCount % LOG_SIZE] = {car.plate, minutesSince(car.entryMs), due, method, millis()};
    ++paymentCount;
    Serial.printf("PAY %s %s via %s\n", car.plate.c_str(), formatChf(due).c_str(), method.c_str());
}

// ---------------------------------------------------------------------------
// API for Pico 2
// ---------------------------------------------------------------------------

void apiEntry() {
    const String plate = normalizePlate(server.arg("plate"));
    if (plate.isEmpty()) {
        server.send(400, "text/plain", "INVALID");
        return;
    }
    if (findCar(plate)) {
        server.send(409, "text/plain", "INSIDE");
        return;
    }
    if (carCount >= CAPACITY) {
        server.send(507, "text/plain", "FULL");
        return;
    }
    Car& car = cars[carCount++];
    car = Car{};
    car.plate = plate;
    car.entryMs = millis();
    Serial.printf("ENTRY %s (%u/%u)\n", plate.c_str(), carCount, CAPACITY);
    server.send(200, "text/plain", "OK");
}

void apiHello() {
    schrankenIp = server.client().remoteIP();
    server.send(200, "text/plain", "OK");
}

void apiStatus() {
    Car* car = findCar(normalizePlate(server.arg("plate")));
    if (!car) {
        server.send(404, "text/plain", "UNKNOWN");
        return;
    }
    const uint32_t due = amountDue(*car);
    server.send(200, "text/plain", due == 0 ? String("PAID") : "UNPAID " + String(due));
}

void apiExit() {
    Car* car = findCar(normalizePlate(server.arg("plate")));
    if (!car) {
        server.send(404, "text/plain", "UNKNOWN");
        return;
    }
    const uint32_t due = amountDue(*car);
    if (due > 0) {
        server.send(402, "text/plain", "UNPAID " + String(due));
        return;
    }
    Serial.printf("EXIT %s\n", car->plate.c_str());
    removeCar(car);
    exitBarrier.openNow();
    server.send(200, "text/plain", "PAID");
}

// ---------------------------------------------------------------------------
// Web page
// ---------------------------------------------------------------------------

void redirectHome(const String& msg, bool ok) {
    server.sendHeader("Location", String("/?") + (ok ? "ok=" : "err=") + urlEncode(msg));
    server.send(303);
}

String statusCell(const Car& car) {
    const uint32_t due = amountDue(car);
    if (due == 0 && car.paid) {
        return "<span class=ok>bezahlt (" + car.paidAt + ")</span>";
    }
    if (due == 0) {
        return "<span class=ok>gratis</span>";
    }
    if (car.paid) {
        return "<span class=bad>Nachzahlung " + formatChf(due) + "</span>";
    }
    return "<span class=bad>offen</span>";
}

String automatQuote(const String& raw) {
    const String plate = normalizePlate(raw);
    if (plate.isEmpty()) {
        return "<div class='msg bad'>Ungültiges Kennzeichen: " + htmlEscape(raw) + "</div>";
    }
    const Car* car = findCar(plate);
    if (!car) {
        return "<div class='msg bad'>" + displayPlate(plate) + " ist nicht im Parkhaus.</div>";
    }
    const uint32_t due = amountDue(*car);
    String q = "<div class=msg><b>" + displayPlate(plate) + "</b> &middot; Parkdauer " +
               formatDuration(minutesSince(car->entryMs)) + "<br>";
    if (due == 0) {
        return q + "<span class=ok>Nichts zu bezahlen, Ausfahrt frei.</span></div>";
    }
    q += "Zu bezahlen: <b>" + formatChf(due) + "</b> ";
    q += "<form action=/pay method=post style=display:inline><input type=hidden name=plate value=" +
         plate + "><input type=hidden name=method value=Automat><button>Bezahlen</button></form>";
    return q + "</div>";
}

void handleRoot() {
    String h;
    h.reserve(6000);
    h += F("<!doctype html><html lang=de><head><meta charset=utf-8><title>Kasse</title>");
    h += FPSTR(PAGE_STYLE);
    h += F("<meta http-equiv=refresh content=15></head><body>");
    h += F("<h1>Kasse</h1><small>Pico 1 &middot; ");
    h += String(carCount) + " / " + String(CAPACITY) + " Plätze belegt";
    if (TIME_FACTOR != 1) {
        h += " &middot; Demo-Zeit: 1 s = " + String(TIME_FACTOR) + " s";
    }
    if (schrankenIp.isSet()) {
        h += " &middot; <a href=http://" + schrankenIp.toString() + "/>zu den Schranken</a>";
    } else {
        h += F(" &middot; Schranken-Pico noch nicht verbunden");
    }
    h += F("</small>");

    if (server.hasArg("ok")) {
        h += "<div class='msg ok'>" + htmlEscape(server.arg("ok")) + "</div>";
    }
    if (server.hasArg("err")) {
        h += "<div class='msg bad'>" + htmlEscape(server.arg("err")) + "</div>";
    }

    // Automat: enter plate -> fee is shown -> confirm payment
    h += F("<h2>Automat</h2><form action=/ method=get>"
           "<input name=plate placeholder='ZH 123456' required> "
           "<button>Gebühr anzeigen</button></form>");
    if (server.hasArg("plate")) {
        h += automatQuote(server.arg("plate"));
    }

    h += F("<h2>Parkierte Fahrzeuge</h2>");
    if (carCount == 0) {
        h += F("<p>Keine Fahrzeuge im Parkhaus.</p>");
    } else {
        h += F("<table><tr><th>Kennzeichen</th><th>Parkdauer</th><th>Gebühr</th>"
               "<th>Status</th><th></th></tr>");
        for (size_t i = 0; i < carCount; ++i) {
            const Car& car = cars[i];
            const uint32_t minutes = minutesSince(car.entryMs);
            const uint32_t due = amountDue(car);
            h += "<tr><td>" + displayPlate(car.plate) + "</td><td>" + formatDuration(minutes) +
                 "</td><td>" + formatChf(feeForMinutes(minutes)) + "</td><td>" + statusCell(car) +
                 "</td><td>";
            if (due > 0) {
                h += "<form action=/pay method=post><input type=hidden name=plate value=" +
                     car.plate +
                     "><input type=hidden name=method value=Kasse><button>" + formatChf(due) +
                     " an Kasse bezahlen</button></form>";
            }
            h += F("</td></tr>");
        }
        h += F("</table>");
    }

    h += F("<h2>Letzte Zahlungen</h2>");
    if (paymentCount == 0) {
        h += F("<p>Noch keine Zahlungen.</p>");
    } else {
        h += F("<table><tr><th>vor</th><th>Kennzeichen</th><th>Parkdauer</th><th>Betrag</th>"
               "<th>Ort</th></tr>");
        const size_t shown = min(paymentCount, LOG_SIZE);
        for (size_t n = 0; n < shown; ++n) {
            const Payment& p = payments[(paymentCount - 1 - n) % LOG_SIZE];
            h += "<tr><td>" + formatDuration((millis() - p.atMs) / 60000) + "</td><td>" +
                 displayPlate(p.plate) + "</td><td>" + formatDuration(p.minutes) + "</td><td>" +
                 formatChf(p.rappen) + "</td><td>" + p.method + "</td></tr>";
        }
        h += F("</table>");
    }

    h += "<h2>Tarif</h2><p>Die ersten " + String(FREE_MINUTES) + " Minuten gratis, danach " +
         formatChf(RATE_PER_HOUR_RAPPEN) + " pro angefangene Stunde, max. " +
         formatChf(DAY_MAX_RAPPEN) + " pro 24 h. Nach dem Bezahlen " +
         String(EXIT_GRACE_MINUTES) + " Minuten Zeit zur Ausfahrt.</p>";
    h += F("</body></html>");
    server.send(200, "text/html; charset=utf-8", h);
}

void handlePay() {
    const String plate = normalizePlate(server.arg("plate"));
    const String method = server.arg("method") == "Automat" ? "Automat" : "Kasse";
    if (plate.isEmpty()) {
        redirectHome("Ungültiges Kennzeichen: " + server.arg("plate"), false);
        return;
    }
    Car* car = findCar(plate);
    if (!car) {
        redirectHome(displayPlate(plate) + " ist nicht im Parkhaus.", false);
        return;
    }
    const uint32_t due = amountDue(*car);
    if (due == 0) {
        redirectHome(displayPlate(plate) + ": nichts zu bezahlen, Ausfahrt frei.", true);
        return;
    }
    pay(*car, method);
    redirectHome(displayPlate(plate) + ": " + formatChf(due) + " bezahlt (" + method +
                     "). Ausfahrt innert " + String(EXIT_GRACE_MINUTES) + " min.",
                 true);
}

// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    exitBarrier.begin();

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(KASSE_IP, KASSE_IP, NETMASK);
    while (!WiFi.softAP(WIFI_SSID, WIFI_PASS)) {
        Serial.println("Access point failed, retrying");
        delay(1000);
    }
    digitalWrite(LED_BUILTIN, HIGH);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/pay", HTTP_POST, handlePay);
    server.on("/api/entry", HTTP_POST, apiEntry);
    server.on("/api/status", HTTP_GET, apiStatus);
    server.on("/api/exit", HTTP_POST, apiExit);
    server.on("/api/hello", HTTP_GET, apiHello);
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
}

void loop() {
    server.handleClient();
    exitBarrier.update();

    static uint32_t lastInfo = 0;
    if (millis() - lastInfo > 10000) {
        lastInfo = millis();
        Serial.printf("Kasse: SSID %s, http://%s/, %u Fahrzeuge, %d WLAN-Clients\n", WIFI_SSID,
                      WiFi.softAPIP().toString().c_str(), carCount, WiFi.softAPgetStationNum());
    }
}
