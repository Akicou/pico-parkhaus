// Pico 1 (Kasse)
//  - Web page Kasse (/): fee lookup and payment, current and completed parking sessions,
//    help requests, test function "Uhrzeit setzen"
//  - Web page Automat (/automat): fee lookup and payment
//  - API used by Pico 2 (Schranken):
//      POST /api/entry?plate=LU123        -> 200 OK | 400 INVALID | 409 INSIDE | 507 FULL
//      GET  /api/status?plate=LU123       -> 200 PAID | UNPAID <rappen> | 404 UNKNOWN
//      POST /api/exit?plate=LU123         -> 200 PAID (exit time saved) | 402 UNPAID <rappen> | 404 UNKNOWN
//      POST /api/help?where=Einfahrt&info -> 200 OK
//      GET  /api/hello                    -> 200 OK, remembers the Schranken address
//
// Parking sessions are kept in RAM only and are lost on restart.

#include <Arduino.h>
#include <WebServer.h>

#include "config.h"
#include "net.h"

struct Parkvorgang {
    String plate;
    uint32_t entryTs = 0;
    uint32_t exitTs = 0;  // 0 = still in the garage
    bool paid = false;
    uint32_t paidRappen = 0;
    uint32_t paidTs = 0;
    String paidAt;  // "Kasse" or "Automat"
};

struct HelpRequest {
    String where;
    String info;
    uint32_t ts;
    bool done;
};

Parkvorgang sessions[MAX_SESSIONS];
size_t sessionCount = 0;

constexpr size_t MAX_HELP = 20;
HelpRequest helpRequests[MAX_HELP];
size_t helpCount = 0;

Net net;
WebServer server(80);
IPAddress schrankenIp;  // learned from /api/hello

// ---------------------------------------------------------------------------
// Clock. Runs from 01.01.2026 00:00 after a restart until it is set on the Kasse page.
// ---------------------------------------------------------------------------

uint32_t clockBase = makeTime(2026, 1, 1, 0, 0);
uint32_t clockSetMs = 0;
bool clockSet = false;

uint32_t clockNow() { return clockBase + (millis() - clockSetMs) / 1000; }

void setClock(uint32_t t) {
    clockBase = t;
    clockSetMs = millis();
    clockSet = true;
    Serial.printf("Uhr gestellt: %s\n", formatDateTime(t).c_str());
}

// ---------------------------------------------------------------------------
// Parking sessions and tariff
// ---------------------------------------------------------------------------

// Whole minutes parked, seconds cut off. 0 if the clock was set back before the entry.
uint32_t parkedMinutes(const Parkvorgang& s) {
    const uint32_t end = s.exitTs ? s.exitTs : clockNow();
    return end > s.entryTs ? (end - s.entryTs) / 60 : 0;
}

// CHF 2.00 per started hour, at least one hour: 0-60 min = 1 h, 61 min = 2 h.
uint32_t feeForMinutes(uint32_t minutes) {
    const uint32_t hours = minutes == 0 ? 1 : (minutes + 59) / 60;
    return hours * RATE_PER_HOUR_RAPPEN;
}

uint32_t amountDue(const Parkvorgang& s) { return s.paid ? 0 : feeForMinutes(parkedMinutes(s)); }

Parkvorgang* findCurrent(const String& plate) {
    for (size_t i = 0; i < sessionCount; ++i) {
        if (sessions[i].exitTs == 0 && sessions[i].plate == plate) {
            return &sessions[i];
        }
    }
    return nullptr;
}

size_t currentCount() {
    size_t n = 0;
    for (size_t i = 0; i < sessionCount; ++i) {
        if (sessions[i].exitTs == 0) {
            ++n;
        }
    }
    return n;
}

// Makes room for a new session by dropping the oldest completed one.
bool makeRoom() {
    if (sessionCount < MAX_SESSIONS) {
        return true;
    }
    for (size_t i = 0; i < sessionCount; ++i) {
        if (sessions[i].exitTs != 0) {
            for (size_t j = i; j + 1 < sessionCount; ++j) {
                sessions[j] = sessions[j + 1];
            }
            sessions[--sessionCount] = Parkvorgang{};
            return true;
        }
    }
    return false;
}

void pay(Parkvorgang& s, const String& method) {
    s.paidRappen = amountDue(s);
    s.paid = true;
    s.paidTs = clockNow();
    s.paidAt = method;
    Serial.printf("ZAHLUNG %s %s (%s)\n", s.plate.c_str(), formatChf(s.paidRappen).c_str(),
                  method.c_str());
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
    if (findCurrent(plate)) {
        server.send(409, "text/plain", "INSIDE");
        return;
    }
    if (currentCount() >= CAPACITY || !makeRoom()) {
        server.send(507, "text/plain", "FULL");
        return;
    }
    Parkvorgang& s = sessions[sessionCount++];
    s = Parkvorgang{};
    s.plate = plate;
    s.entryTs = clockNow();
    Serial.printf("EINFAHRT %s %s\n", plate.c_str(), formatDateTime(s.entryTs).c_str());
    server.send(200, "text/plain", "OK");
}

void apiStatus() {
    const Parkvorgang* s = findCurrent(normalizePlate(server.arg("plate")));
    if (!s) {
        server.send(404, "text/plain", "UNKNOWN");
        return;
    }
    server.send(200, "text/plain", s->paid ? String("PAID") : "UNPAID " + String(amountDue(*s)));
}

void apiExit() {
    Parkvorgang* s = findCurrent(normalizePlate(server.arg("plate")));
    if (!s) {
        server.send(404, "text/plain", "UNKNOWN");
        return;
    }
    if (!s->paid) {
        server.send(402, "text/plain", "UNPAID " + String(amountDue(*s)));
        return;
    }
    s->exitTs = clockNow();
    Serial.printf("AUSFAHRT %s %s\n", s->plate.c_str(), formatDateTime(s->exitTs).c_str());
    server.send(200, "text/plain", "PAID");
}

void apiHelp() {
    HelpRequest& h = helpRequests[helpCount % MAX_HELP];
    h = {server.arg("where") == "Ausfahrt" ? "Ausfahrt" : "Einfahrt", server.arg("info"), clockNow(),
         false};
    ++helpCount;
    Serial.printf("HILFE %s: %s\n", h.where.c_str(), h.info.c_str());
    server.send(200, "text/plain", "OK");
}

void apiHello() {
    schrankenIp = server.client().remoteIP();
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Web pages
// ---------------------------------------------------------------------------

void redirect(const String& page, const String& msg, bool ok) {
    server.sendHeader("Location", page + "?" + (ok ? "ok=" : "err=") + urlEncode(msg));
    server.send(303);
}

String pageHead(const char* title) {
    String h;
    h.reserve(8000);
    h += F("<!doctype html><html lang=de><head><meta charset=utf-8><title>");
    h += title;
    h += F("</title>");
    h += FPSTR(PAGE_STYLE);
    h += F("</head><body><h1>");
    h += title;
    h += "</h1><small>Pico 1 &middot; Uhrzeit Pico 1: <b>" + formatDateTime(clockNow()) + "</b>";
    if (!clockSet) {
        h += F(" <span class=bad>(nicht gestellt)</span>");
    }
    h += F(" &middot; <a href=/>Kasse</a> &middot; <a href=/automat>Automat</a> &middot; ");
    if (schrankenIp.isSet()) {
        h += "<a href=http://" + schrankenIp.toString() + "/>Schranken</a>";
    } else {
        h += F("Schranken noch nicht verbunden");
    }
    h += F("</small>");

    if (server.hasArg("ok")) {
        h += "<div class='msg ok'>" + htmlEscape(server.arg("ok")) + "</div>";
    }
    if (server.hasArg("err")) {
        h += "<div class='msg bad'>" + htmlEscape(server.arg("err")) + "</div>";
    }
    return h;
}

// Fee lookup form plus the result for ?plate=... (shared by Kasse and Automat).
String feeLookup(const char* page, const char* method) {
    String h = "<h2>Betrag abfragen</h2><form action=" + String(page) +
               " method=get><input name=plate placeholder='LU 123456'> "
               "<button>Betrag abfragen</button></form>";
    if (!server.hasArg("plate")) {
        return h;
    }
    const String raw = server.arg("plate");
    const String plate = normalizePlate(raw);
    if (plate.isEmpty()) {
        return h + "<div class='msg bad'>Ungültiges Kennzeichen: " + htmlEscape(raw) + "</div>";
    }
    const Parkvorgang* s = findCurrent(plate);
    if (!s) {
        return h + "<div class='msg bad'>" + displayPlate(plate) + " ist nicht im Parkhaus.</div>";
    }
    h += "<div class=msg><b>" + displayPlate(plate) + "</b><br>Einfahrt: " +
         formatDateTime(s->entryTs) + "<br>Parkdauer: " + formatDuration(parkedMinutes(*s)) +
         "<br>";
    if (s->paid) {
        return h + "<span class=ok>Bereits bezahlt (" + formatChf(s->paidRappen) + ", " +
               s->paidAt + "). Kein weiterer Betrag.</span></div>";
    }
    h += "Betrag: <b>" + formatChf(amountDue(*s)) + "</b><br><br>";
    h += "<form action=/pay method=post><input type=hidden name=plate value=" + plate +
         "><input type=hidden name=method value=" + method + "><button>Bezahlen</button></form>";
    return h + "</div>";
}

void handleKasse() {
    String h = pageHead("Kasse");
    h += feeLookup("/", "Kasse");

    // Help requests from the Schranken
    String help;
    const size_t shownHelp = min(helpCount, MAX_HELP);
    for (size_t n = 0; n < shownHelp; ++n) {
        const size_t idx = (helpCount - 1 - n) % MAX_HELP;
        const HelpRequest& r = helpRequests[idx];
        if (r.done) {
            continue;
        }
        help += "<tr><td>" + formatDateTime(r.ts) + "</td><td>" + r.where + "</td><td>" +
                htmlEscape(r.info) + "</td><td><form action=/help/done method=post>" +
                "<input type=hidden name=id value=" + String(idx) +
                "><button>Erledigt</button></form></td></tr>";
    }
    if (!help.isEmpty()) {
        h += F("<h2 class=bad>Hilfeanfragen</h2><table><tr><th>Zeit</th><th>Ort</th>"
               "<th>Eingabe</th><th></th></tr>");
        h += help + "</table>";
    }

    h += "<h2>Aktuelle Parkvorgänge (" + String(currentCount()) + " / " + String(CAPACITY) +
         ")</h2>";
    h += F("<table><tr><th>Kennzeichen</th><th>Einfahrt</th><th>Parkdauer</th><th>Betrag</th>"
           "<th>Status</th></tr>");
    for (size_t i = 0; i < sessionCount; ++i) {
        const Parkvorgang& s = sessions[i];
        if (s.exitTs) {
            continue;
        }
        h += "<tr><td><a href='/?plate=" + s.plate + "'>" + displayPlate(s.plate) + "</a></td><td>" +
             formatDateTime(s.entryTs) + "</td><td>" + formatDuration(parkedMinutes(s)) + "</td><td>";
        if (s.paid) {
            h += formatChf(s.paidRappen) + "</td><td class=ok>bezahlt (" + s.paidAt + ")";
        } else {
            h += formatChf(amountDue(s)) + "</td><td class=bad>unbezahlt";
        }
        h += F("</td></tr>");
    }
    h += F("</table>");

    h += F("<h2>Abgeschlossene Parkvorgänge</h2><table><tr><th>Kennzeichen</th><th>Einfahrt</th>"
           "<th>Ausfahrt</th><th>Parkdauer</th><th>Bezahlt</th></tr>");
    for (size_t i = 0; i < sessionCount; ++i) {
        const Parkvorgang& s = sessions[i];
        if (!s.exitTs) {
            continue;
        }
        h += "<tr><td>" + displayPlate(s.plate) + "</td><td>" + formatDateTime(s.entryTs) +
             "</td><td>" + formatDateTime(s.exitTs) + "</td><td>" +
             formatDuration(parkedMinutes(s)) + "</td><td class=ok>" + formatChf(s.paidRappen) +
             " (" + s.paidAt + ")</td></tr>";
    }
    h += F("</table>");

    h += "<h2>Testfunktion: Uhrzeit setzen</h2><form action=/clock method=post>"
         "<input type=datetime-local name=t required value=" +
         formatDateTimeInput(clockNow()) + "> <button>Uhrzeit setzen</button></form>";

    h += "<h2>Tarif</h2><p>" + formatChf(RATE_PER_HOUR_RAPPEN) +
         " pro angefangene Stunde, mindestens eine Stunde. Gespeicherte Parkvorgänge gehen bei "
         "einem Neustart verloren.</p>";
    h += F("</body></html>");
    server.send(200, "text/html; charset=utf-8", h);
}

void handleAutomat() {
    String h = pageHead("Automat");
    h += feeLookup("/automat", "Automat");
    h += F("</body></html>");
    server.send(200, "text/html; charset=utf-8", h);
}

void handlePay() {
    const String method = server.arg("method") == "Automat" ? "Automat" : "Kasse";
    const String page = method == "Automat" ? "/automat" : "/";
    const String plate = normalizePlate(server.arg("plate"));
    Parkvorgang* s = plate.isEmpty() ? nullptr : findCurrent(plate);
    if (!s) {
        redirect(page, "Kein Parkvorgang für " + server.arg("plate") + " gefunden.", false);
        return;
    }
    if (s->paid) {
        redirect(page, displayPlate(plate) + " ist bereits bezahlt.", false);
        return;
    }
    pay(*s, method);
    redirect(page, displayPlate(plate) + ": " + formatChf(s->paidRappen) + " bezahlt (" + method +
                       "). Gute Fahrt!",
             true);
}

void handleClock() {
    const uint32_t t = parseDateTimeInput(server.arg("t"));
    if (t == 0) {
        redirect("/", "Ungültige Uhrzeit.", false);
        return;
    }
    setClock(t);
    redirect("/", "Uhrzeit gesetzt auf " + formatDateTime(t) + ".", true);
}

void handleHelpDone() {
    const long id = server.arg("id").toInt();
    if (id >= 0 && static_cast<size_t>(id) < MAX_HELP) {
        helpRequests[id].done = true;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    net.begin(KASSE_HOSTNAME);

    server.on("/", HTTP_GET, handleKasse);
    server.on("/automat", HTTP_GET, handleAutomat);
    server.on("/pay", HTTP_POST, handlePay);
    server.on("/clock", HTTP_POST, handleClock);
    server.on("/help/done", HTTP_POST, handleHelpDone);
    server.on("/api/entry", HTTP_POST, apiEntry);
    server.on("/api/status", HTTP_GET, apiStatus);
    server.on("/api/exit", HTTP_POST, apiExit);
    server.on("/api/help", HTTP_POST, apiHelp);
    server.on("/api/hello", HTTP_GET, apiHello);
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
}

void loop() {
    server.handleClient();
    net.loop();
}
