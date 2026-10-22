#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Network. Pico 1 (Kasse) opens this Wi-Fi network, Pico 2 (Schranken) joins it.
// Connect your laptop/phone to the same network and open the Kasse at http://192.168.42.1/
// The Schranken Pico gets its address via DHCP (a static IP does not work in station mode
// with this core) and announces it to the Kasse, whose page links to it.
// Give each group its own SSID so neighbouring setups don't collide.
// ---------------------------------------------------------------------------
#define WIFI_SSID "Parkhaus-Gruppe1"
#define WIFI_PASS "parkhaus123"  // min. 8 characters

static const IPAddress KASSE_IP(192, 168, 42, 1);
static const IPAddress NETMASK(255, 255, 255, 0);

// ---------------------------------------------------------------------------
// Tariff (Kasse). Fee = started hours after the free period * rate, capped per day.
// ---------------------------------------------------------------------------
constexpr uint32_t FREE_MINUTES = 15;          // first 15 min free
constexpr uint32_t RATE_PER_HOUR_RAPPEN = 200;  // CHF 2.00 per started hour
constexpr uint32_t DAY_MAX_RAPPEN = 2000;       // CHF 20.00 max per 24 h
constexpr uint32_t EXIT_GRACE_MINUTES = 15;     // time to leave after paying
constexpr size_t CAPACITY = 50;                 // parking spaces

// 1 = real time. 60 = one real second counts as one parking minute, so a
// 2 h stay can be tested in 2 minutes. Shown on both web pages.
constexpr uint32_t TIME_FACTOR = 1;

// ---------------------------------------------------------------------------
// Barriers (Schranken).
// ---------------------------------------------------------------------------
constexpr uint32_t BARRIER_OPEN_MS = 5000;  // close automatically after 5 s

// Each Pico drives one SG90 on GP8 (physical pin 11):
//   Einfahrt (left)  -> Pico 2 Schranken
//   Ausfahrt (right) -> Pico 1 Kasse, opened by the Kasse when it approves an exit
// Pulse widths in microseconds, measured on the real arms.
constexpr uint8_t BARRIER_SERVO_PIN = 8;
constexpr uint16_t ENTRY_OPEN_US = 500;
constexpr uint16_t ENTRY_CLOSED_US = 2000;
constexpr uint16_t EXIT_OPEN_US = 1000;
constexpr uint16_t EXIT_CLOSED_US = 2000;

// ---------------------------------------------------------------------------
// Swiss plate: canton code + 1..6 digits, no leading zero, e.g. "ZH 123456".
// normalizePlate() uppercases and drops spaces, dots and dashes ("zh-12 34" -> "ZH1234").
// Returns an empty string if the plate is invalid.
// ---------------------------------------------------------------------------
inline String normalizePlate(const String& raw) {
    static const char* const CANTONS[] = {
        "AG", "AI", "AR", "BE", "BL", "BS", "FR", "GE", "GL", "GR", "JU", "LU", "NE",
        "NW", "OW", "SG", "SH", "SO", "SZ", "TG", "TI", "UR", "VD", "VS", "ZG", "ZH"};

    String p;
    for (char c : raw) {
        if (c == ' ' || c == '-' || c == '.') {
            continue;
        }
        p += static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    if (p.length() < 3 || p.length() > 8) {
        return "";
    }
    const String canton = p.substring(0, 2);
    bool cantonOk = false;
    for (const char* c : CANTONS) {
        if (canton == c) {
            cantonOk = true;
        }
    }
    if (!cantonOk || p[2] == '0') {
        return "";
    }
    for (size_t i = 2; i < p.length(); ++i) {
        if (!isDigit(p[i])) {
            return "";
        }
    }
    return p;
}

// "ZH123456" -> "ZH 123456"
inline String displayPlate(const String& p) { return p.substring(0, 2) + " " + p.substring(2); }

inline String formatChf(uint32_t rappen) {
    char buf[16];
    snprintf(buf, sizeof(buf), "CHF %lu.%02lu", static_cast<unsigned long>(rappen / 100),
             static_cast<unsigned long>(rappen % 100));
    return buf;
}

inline String formatDuration(uint32_t minutes) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%luh %02lumin", static_cast<unsigned long>(minutes / 60),
             static_cast<unsigned long>(minutes % 60));
    return buf;
}

// Parking minutes elapsed since a millis() timestamp, scaled by TIME_FACTOR.
inline uint32_t minutesSince(uint32_t startMs) {
    return static_cast<uint32_t>((static_cast<uint64_t>(millis() - startMs) * TIME_FACTOR) / 60000);
}

inline String urlEncode(const String& s) {
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String out;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (isAlphaNumeric(u) || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            out += '%';
            out += HEX_DIGITS[u >> 4];
            out += HEX_DIGITS[u & 0x0F];
        }
    }
    return out;
}

inline String htmlEscape(const String& s) {
    String out;
    for (char c : s) {
        switch (c) {
            case '&': out += F("&amp;"); break;
            case '<': out += F("&lt;"); break;
            case '>': out += F("&gt;"); break;
            case '"': out += F("&quot;"); break;
            case '\'': out += F("&#39;"); break;
            default: out += c;
        }
    }
    return out;
}

// Shared page style for both web interfaces.
static const char PAGE_STYLE[] PROGMEM = R"CSS(
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:system-ui,sans-serif;max-width:860px;margin:2em auto;padding:0 1em;color:#222}
h1{margin-bottom:.2em}small{color:#666}
table{border-collapse:collapse;width:100%;margin:1em 0}
th,td{border-bottom:1px solid #ddd;padding:.45em;text-align:left}
input,button{font-size:1em;padding:.45em .7em}
button{cursor:pointer}
.ok{color:#11752a;font-weight:600}.bad{color:#b3261e;font-weight:600}
.msg{padding:.7em 1em;margin:1em 0;border-left:4px solid #888;background:#f4f4f4}
.msg.ok{border-color:#11752a}.msg.bad{border-color:#b3261e}
.barrier{display:inline-block;min-width:12em;padding:.6em 1em;margin:.3em .6em .3em 0;border:2px solid #888}
.barrier.open{border-color:#11752a;color:#11752a}.barrier.closed{border-color:#b3261e;color:#b3261e}
</style>
)CSS";
