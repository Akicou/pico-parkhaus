// Pico 2 (Schranken)
//  - Web page = camera simulation: one field for Einfahrt, one for Ausfahrt
//  - Einfahrt: valid plate registered at the Kasse -> entry barrier opens
//  - Ausfahrt: Kasse confirms payment -> exit barrier opens
//  - Error messages offer "Hilfe anfordern", which shows up on the Kasse page
//  - Barriers close automatically after BARRIER_OPEN_MS
//
// Finds the Kasse as KASSE_HOSTNAME.local (mDNS). If the hotspot blocks mDNS, the Kasse IP
// can be entered on the web page (the Kasse prints it on its serial monitor).

#include <Arduino.h>
#include <HTTPClient.h>
#include <WebServer.h>

#include "barrier.h"
#include "config.h"
#include "net.h"

Barrier entryBarrier{"Einfahrt", ENTRY_SERVO_PIN, ENTRY_OPEN_US, ENTRY_CLOSED_US};
Barrier exitBarrier{"Ausfahrt", EXIT_SERVO_PIN, EXIT_OPEN_US, EXIT_CLOSED_US};

Net net;
WebServer server(80);

IPAddress kasseIp;        // resolved via mDNS, cleared when the Kasse stops answering
IPAddress kasseManualIp;  // entered on the web page, wins over mDNS

struct Event {
    uint32_t atMs;
    String text;
    bool ok;
};
constexpr size_t LOG_SIZE = 20;
Event events[LOG_SIZE];
size_t eventCount = 0;

// Last camera result, shown at the top of the page. A failed one offers "Hilfe anfordern".
struct Result {
    String text;
    bool ok = true;
    String where;  // "Einfahrt" / "Ausfahrt"
    String input;  // what the operator typed
    bool helpSent = false;
} last;

void logEvent(const String& text, bool ok) {
    events[eventCount % LOG_SIZE] = {millis(), text, ok};
    ++eventCount;
    Serial.println(text);
}

void setResult(const String& where, const String& input, const String& text, bool ok) {
    last = {where + ": " + text, ok, where, input, false};
    logEvent(last.text, ok);
}

// ---------------------------------------------------------------------------
// Kasse API
// ---------------------------------------------------------------------------

struct KasseReply {
    int code;     // HTTP status, or < 0 if the Kasse could not be reached
    String body;  // "OK", "INSIDE", "PAID", "UNPAID 400", ...
};

bool findKasse() {
    if (kasseManualIp.isSet()) {
        kasseIp = kasseManualIp;
        return true;
    }
    if (kasseIp.isSet()) {
        return true;
    }
    IPAddress ip;
    if (hostByName(KASSE_HOSTNAME ".local", ip, 1500) && ip.isSet()) {
        kasseIp = ip;
        Serial.printf("Kasse gefunden: %s\n", ip.toString().c_str());
        return true;
    }
    return false;
}

KasseReply callKasse(const char* method, const String& pathAndQuery) {
    if (WiFi.status() != WL_CONNECTED) {
        return {-1, "kein WLAN"};
    }
    if (!findKasse()) {
        return {-1, "Kasse nicht gefunden"};
    }
    HTTPClient http;
    http.setTimeout(KASSE_TIMEOUT_MS);
    if (!http.begin("http://" + kasseIp.toString() + pathAndQuery)) {
        return {-1, "Verbindungsfehler"};
    }
    const int code = String(method) == "POST" ? http.POST("") : http.GET();
    KasseReply reply{code, code > 0 ? http.getString() : String("keine Antwort")};
    http.end();
    reply.body.trim();
    if (code <= 0) {
        kasseIp = IPAddress();  // look it up again next time (new DHCP address after restart)
    }
    return reply;
}

// "UNPAID 400" -> "CHF 4.00"
String unpaidAmount(const String& body) { return formatChf(body.substring(7).toInt()); }

// ---------------------------------------------------------------------------
// Camera simulation
// ---------------------------------------------------------------------------

void handleEntry(const String& raw) {
    String input = raw;
    input.trim();
    const String plate = normalizePlate(input);
    if (input.isEmpty()) {
        setResult("Einfahrt", input, "Kein Kennzeichen erkannt. Schranke bleibt geschlossen.", false);
        return;
    }
    if (plate.isEmpty()) {
        setResult("Einfahrt", input,
                  "Ungültiges Kennzeichen \"" + input + "\". Schranke bleibt geschlossen.", false);
        return;
    }
    const KasseReply r = callKasse("POST", "/api/entry?plate=" + urlEncode(plate));
    const String p = displayPlate(plate);
    if (r.code == 200) {
        entryBarrier.openNow();
        setResult("Einfahrt", input, p + " registriert. Schranke offen.", true);
    } else if (r.code == 409) {
        setResult("Einfahrt", input, p + " ist bereits im Parkhaus. Schranke bleibt geschlossen.",
                  false);
    } else if (r.code == 507) {
        setResult("Einfahrt", input, "Parkhaus voll. Schranke bleibt geschlossen.", false);
    } else {
        setResult("Einfahrt", input,
                  "Kasse nicht erreichbar (" + r.body + "). Schranke bleibt geschlossen.", false);
    }
}

void handleExit(const String& raw) {
    String input = raw;
    input.trim();
    const String plate = normalizePlate(input);
    if (input.isEmpty()) {
        setResult("Ausfahrt", input, "Kein Kennzeichen erkannt. Schranke bleibt geschlossen.", false);
        return;
    }
    if (plate.isEmpty()) {
        setResult("Ausfahrt", input,
                  "Ungültiges Kennzeichen \"" + input + "\". Schranke bleibt geschlossen.", false);
        return;
    }
    const KasseReply r = callKasse("POST", "/api/exit?plate=" + urlEncode(plate));
    const String p = displayPlate(plate);
    if (r.code == 200 && r.body == "PAID") {
        exitBarrier.openNow();
        setResult("Ausfahrt", input, p + " bezahlt. Schranke offen.", true);
    } else if (r.code == 402) {
        setResult("Ausfahrt", input,
                  p + ": Parkgebühr offen (" + unpaidAmount(r.body) +
                      "). Bitte an Kasse oder Automat bezahlen. Schranke bleibt geschlossen.",
                  false);
    } else if (r.code == 404) {
        setResult("Ausfahrt", input,
                  "Kein Parkvorgang für " + p + " gefunden. Schranke bleibt geschlossen.", false);
    } else {
        setResult("Ausfahrt", input,
                  "Kasse nicht erreichbar (" + r.body + "). Schranke bleibt geschlossen.", false);
    }
}

void handleCamera() {
    if (server.arg("dir") == "in") {
        handleEntry(server.arg("plate"));
    } else {
        handleExit(server.arg("plate"));
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleHelp() {
    if (!last.ok && !last.helpSent) {
        const KasseReply r = callKasse("POST", "/api/help?where=" + urlEncode(last.where) +
                                                   "&info=" + urlEncode(last.input));
        if (r.code == 200) {
            last.helpSent = true;
            logEvent(last.where + ": Hilfe angefordert", true);
        } else {
            logEvent(last.where + ": Hilfe konnte nicht angefordert werden, Kasse nicht erreichbar",
                     false);
        }
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleKasseIp() {
    IPAddress ip;
    if (server.arg("ip").isEmpty()) {
        kasseManualIp = IPAddress();
        kasseIp = IPAddress();
    } else if (ip.fromString(server.arg("ip"))) {
        kasseManualIp = ip;
        kasseIp = ip;
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

// Barrier state for the live display on the page.
void handleState() {
    server.send(200, "application/json",
                String("{\"in\":") + (entryBarrier.open ? "true" : "false") +
                    ",\"out\":" + (exitBarrier.open ? "true" : "false") + "}");
}

String barrierBox(const char* id, const char* name, bool open) {
    return "<div id=" + String(id) + " class='barrier " + (open ? "open" : "closed") +
           "'>Schranke " + name + ": <b>" + (open ? "offen" : "geschlossen") + "</b></div>";
}

void handleRoot() {
    String h;
    h.reserve(6000);
    h += F("<!doctype html><html lang=de><head><meta charset=utf-8><title>Schranken</title>");
    h += FPSTR(PAGE_STYLE);
    h += F("</head><body><h1>Schranken</h1><small>Pico 2 &middot; Kamera-Simulation &middot; ");
    if (kasseIp.isSet()) {
        h += "<a href=http://" + kasseIp.toString() + "/>Kasse</a> <span class=ok>verbunden</span>";
    } else {
        h += F("Kasse <span class=bad>noch nicht gefunden</span>");
    }
    h += F("</small>");

    if (!last.text.isEmpty()) {
        h += "<div class='msg " + String(last.ok ? "ok" : "bad") + "'>" + htmlEscape(last.text);
        if (!last.ok && !last.helpSent) {
            h += F("<form action=/help method=post style=margin-top:.6em>"
                   "<button>Hilfe anfordern</button></form>");
        } else if (last.helpSent) {
            h += F("<br>Hilfe wurde angefordert.");
        }
        h += F("</div>");
    }

    h += F("<h2>Einfahrt</h2><form action=/camera method=post>"
           "<input type=hidden name=dir value=in>"
           "<input name=plate placeholder='LU 123456' autofocus> <button>Bestätigen</button></form>"
           "<h2>Ausfahrt</h2><form action=/camera method=post>"
           "<input type=hidden name=dir value=out>"
           "<input name=plate placeholder='LU 123456'> <button>Bestätigen</button></form>");

    h += F("<h2>Schranken</h2>");
    h += barrierBox("in", "Einfahrt", entryBarrier.open) + barrierBox("out", "Ausfahrt", exitBarrier.open);

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

    h += "<h2>Kasse-Adresse</h2><p>Wird automatisch gesucht (" KASSE_HOSTNAME
         ".local). Falls das nicht klappt, IP der Kasse eingeben (leer = automatisch):</p>"
         "<form action=/kasse-ip method=post><input name=ip placeholder='192.168.x.x' value='" +
         (kasseManualIp.isSet() ? kasseManualIp.toString() : String()) +
         "'> <button>Speichern</button></form>";

    // Live barrier state without reloading the page (keeps typed input).
    h += F("<script>"
           "function show(id,open,name){var e=document.getElementById(id);"
           "e.className='barrier '+(open?'open':'closed');"
           "e.innerHTML='Schranke '+name+': <b>'+(open?'offen':'geschlossen')+'</b>';}"
           "setInterval(function(){fetch('/state').then(function(r){return r.json();})"
           ".then(function(s){show('in',s.in,'Einfahrt');show('out',s.out,'Ausfahrt');})"
           ".catch(function(){});},1000);"
           "</script></body></html>");
    server.send(200, "text/html; charset=utf-8", h);
}

// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    entryBarrier.begin();
    exitBarrier.begin();
    net.begin(SCHRANKEN_HOSTNAME);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/camera", HTTP_POST, handleCamera);
    server.on("/help", HTTP_POST, handleHelp);
    server.on("/kasse-ip", HTTP_POST, handleKasseIp);
    server.on("/state", HTTP_GET, handleState);
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
}

void loop() {
    server.handleClient();
    entryBarrier.update();
    exitBarrier.update();
    net.loop();

    // Tell the Kasse where we are, so its page can link here.
    static uint32_t lastHello = 0;
    if (WiFi.status() == WL_CONNECTED && (lastHello == 0 || millis() - lastHello > 30000)) {
        lastHello = millis();
        callKasse("GET", "/api/hello");
    }
}
