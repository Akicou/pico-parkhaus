// Pico 2 (Schranken)
//  - Joins the Kasse Wi-Fi via DHCP and announces its address (GET /api/hello), so the
//    Kasse page can link here
//  - Web page = camera simulation: type a plate, press Einfahrt or Ausfahrt
//  - Einfahrt: plate valid + registered at Kasse -> entry barrier opens
//  - Ausfahrt: Kasse confirms payment -> exit barrier opens
//  - Barriers close automatically after BARRIER_OPEN_MS
//
// Hardware: SG90 servo for the entry barrier (Einfahrt, left) on GP8 (pin 11).
// The exit barrier (Ausfahrt, right) hangs on the Kasse Pico; the Kasse opens it when it
// approves an exit, this page only shows its state.
// The onboard LED is on while connected to the Kasse.

#include <Arduino.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include "barrier.h"
#include "config.h"

Barrier entryBarrier{"Einfahrt", BARRIER_SERVO_PIN, ENTRY_OPEN_US, ENTRY_CLOSED_US};
uint32_t exitOpenedMs = 0;  // when the Kasse last opened the exit barrier, 0 = never

bool exitShownOpen() { return exitOpenedMs && millis() - exitOpenedMs < BARRIER_OPEN_MS; }

struct Event {
    uint32_t atMs;
    String text;
    bool ok;
};
constexpr size_t LOG_SIZE = 20;
Event events[LOG_SIZE];
size_t eventCount = 0;

WebServer server(80);

void logEvent(const String& text, bool ok) {
    events[eventCount % LOG_SIZE] = {millis(), text, ok};
    ++eventCount;
    Serial.println(text);
}

// ---------------------------------------------------------------------------
// Kasse API
// ---------------------------------------------------------------------------

struct KasseReply {
    int code;     // HTTP status, or < 0 if the Kasse could not be reached
    String body;  // "OK", "INSIDE", "PAID", "UNPAID 400", ...
};

KasseReply callKasse(const char* method, const char* path, const String& plate) {
    if (WiFi.status() != WL_CONNECTED) {
        return {-1, "kein WLAN"};
    }
    HTTPClient http;
    http.setTimeout(3000);
    const String url = "http://" + KASSE_IP.toString() + path + "?plate=" + urlEncode(plate);
    if (!http.begin(url)) {
        return {-1, "begin failed"};
    }
    const int code = String(method) == "POST" ? http.POST("") : http.GET();
    KasseReply reply{code, code > 0 ? http.getString() : http.errorToString(code)};
    http.end();
    reply.body.trim();
    return reply;
}

// "UNPAID 400" -> "CHF 4.00"
String unpaidAmount(const String& body) { return formatChf(body.substring(7).toInt()); }

// ---------------------------------------------------------------------------
// Camera simulation
// ---------------------------------------------------------------------------

void handleEntry(const String& plate) {
    const KasseReply r = callKasse("POST", "/api/entry", plate);
    const String p = displayPlate(plate);
    if (r.code == 200) {
        entryBarrier.openNow();
        logEvent("Einfahrt " + p + ": registriert, Schranke offen", true);
    } else if (r.code == 409) {
        logEvent("Einfahrt " + p + ": abgelehnt, Fahrzeug ist bereits im Parkhaus", false);
    } else if (r.code == 507) {
        logEvent("Einfahrt " + p + ": abgelehnt, Parkhaus voll", false);
    } else if (r.code == 400) {
        logEvent("Einfahrt " + p + ": abgelehnt, Kasse meldet ungültiges Kennzeichen", false);
    } else {
        logEvent("Einfahrt " + p + ": Kasse nicht erreichbar (" + r.body + "), Schranke bleibt zu",
                 false);
    }
}

void handleExit(const String& plate) {
    const KasseReply r = callKasse("POST", "/api/exit", plate);
    const String p = displayPlate(plate);
    if (r.code == 200 && r.body == "PAID") {
        exitOpenedMs = millis();  // the Kasse moves the arm
        logEvent("Ausfahrt " + p + ": bezahlt, Schranke offen", true);
    } else if (r.code == 402) {
        logEvent("Ausfahrt " + p + ": nicht bezahlt, offen " + unpaidAmount(r.body), false);
    } else if (r.code == 404) {
        logEvent("Ausfahrt " + p + ": Fahrzeug nicht im Parkhaus registriert", false);
    } else {
        logEvent("Ausfahrt " + p + ": Kasse nicht erreichbar (" + r.body + "), Schranke bleibt zu",
                 false);
    }
}

void camera(const String& raw, bool entry) {
    const String plate = normalizePlate(raw);
    if (plate.isEmpty()) {
        logEvent(String(entry ? "Einfahrt" : "Ausfahrt") + " \"" + raw +
                     "\": ungültiges Kennzeichen, Schranke bleibt zu",
                 false);
    } else if (entry) {
        handleEntry(plate);
    } else {
        handleExit(plate);
    }
}

void handleCamera() {
    camera(server.arg("plate"), server.arg("dir") == "in");
    server.sendHeader("Location", "/");
    server.send(303);
}

String barrierBox(const char* name, bool open) {
    return "<div class='barrier " + String(open ? "open" : "closed") + "'>Schranke " + name +
           ": <b>" + (open ? "offen" : "geschlossen") + "</b></div>";
}

void handleRoot() {
    String h;
    h.reserve(5000);
    h += F("<!doctype html><html lang=de><head><meta charset=utf-8><title>Schranken</title>");
    h += FPSTR(PAGE_STYLE);
    h += F("<meta http-equiv=refresh content=3></head><body>");
    h += F("<h1>Schranken</h1><small>Pico 2 &middot; Kamera-Simulation &middot; Kasse ");
    h += WiFi.status() == WL_CONNECTED ? F("<span class=ok>verbunden</span>")
                                       : F("<span class=bad>nicht verbunden</span>");
    h += " &middot; <a href=http://" + KASSE_IP.toString() + "/>zur Kasse</a></small>";

    h += F("<h2>Kennzeichen erfassen</h2><form action=/camera method=post>"
           "<input name=plate placeholder='ZH 123456' required autofocus> "
           "<button name=dir value=in>Einfahrt</button> "
           "<button name=dir value=out>Ausfahrt</button></form>");

    h += F("<h2>Schranken</h2>");
    h += barrierBox("Einfahrt", entryBarrier.open) + barrierBox("Ausfahrt", exitShownOpen());

    h += F("<h2>Ereignisse</h2>");
    if (eventCount == 0) {
        h += F("<p>Noch keine Ereignisse.</p>");
    } else {
        h += F("<table><tr><th>vor</th><th>Ereignis</th></tr>");
        const size_t shown = min(eventCount, LOG_SIZE);
        for (size_t n = 0; n < shown; ++n) {
            const Event& e = events[(eventCount - 1 - n) % LOG_SIZE];
            h += "<tr><td>" + String((millis() - e.atMs) / 1000) + " s</td><td class=" +
                 (e.ok ? "ok" : "bad") + ">" + htmlEscape(e.text) + "</td></tr>";
        }
        h += F("</table>");
    }
    h += F("</body></html>");
    server.send(200, "text/html; charset=utf-8", h);
}

// ---------------------------------------------------------------------------

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
    entryBarrier.begin();

    connectWifi();

    server.on("/", HTTP_GET, handleRoot);
    server.on("/camera", HTTP_POST, handleCamera);
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
}

void loop() {
    server.handleClient();
    entryBarrier.update();

    // Reconnect if the Kasse was off or restarted. begin() does not block after WiFi.mode(),
    // so give each attempt 30 s before starting over.
    static uint32_t lastCheck = 0;
    static uint32_t lastAttempt = 0;
    if (millis() - lastCheck > 5000) {
        lastCheck = millis();
        const bool connected = WiFi.status() == WL_CONNECTED;
        digitalWrite(LED_BUILTIN, connected ? HIGH : LOW);
        if (!connected && millis() - lastAttempt > 30000) {
            lastAttempt = millis();
            Serial.printf("Schranken: verbinde mit %s ...\n", WIFI_SSID);
            WiFi.disconnect();
            connectWifi();
        } else if (!connected) {
            Serial.println("Schranken: warte auf WLAN ...");
        } else {
            Serial.printf("Schranken: http://%s/  Kasse %s\n", WiFi.localIP().toString().c_str(),
                          KASSE_IP.toString().c_str());
            // Tell the Kasse where we are (it restarts with an empty memory, so keep repeating).
            static uint32_t lastHello = 0;
            if (lastHello == 0 || millis() - lastHello > 30000) {
                if (callKasse("GET", "/api/hello", "").code == 200) {
                    lastHello = millis();
                }
            }
        }
    }
}
