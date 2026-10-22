#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// WLAN. Both Picos join the same hotspot; the first one that is found is used.
// ---------------------------------------------------------------------------
struct WifiNetwork {
    const char* ssid;
    const char* pass;
};
static const WifiNetwork WIFI_NETWORKS[] = {
    {"NullPointerException", "CHANGE_ME"},
    {"No!", "CHANGE_ME"},
};

// The Kasse is reachable as http://parkhaus-kasse.local/ (mDNS), the Schranken as
// http://parkhaus-schranken.local/. Both also print their IP on the serial monitor.
#define KASSE_HOSTNAME "parkhaus-kasse"
#define SCHRANKEN_HOSTNAME "parkhaus-schranken"

// Pico 2 waits at most this long for an answer from the Kasse.
constexpr uint16_t KASSE_TIMEOUT_MS = 2500;

// ---------------------------------------------------------------------------
// Tarif: CHF 2.00 per started hour, at least one hour (0 minutes = CHF 2.00).
// ---------------------------------------------------------------------------
constexpr uint32_t RATE_PER_HOUR_RAPPEN = 200;
constexpr size_t CAPACITY = 50;       // parking spaces
constexpr size_t MAX_SESSIONS = 100;  // current + completed parking sessions kept in RAM

// ---------------------------------------------------------------------------
// Schranken: both SG90 servos on Pico 2. Pulse widths in microseconds.
// ---------------------------------------------------------------------------
constexpr uint32_t BARRIER_OPEN_MS = 5000;  // closes again after 5 s

constexpr uint8_t ENTRY_SERVO_PIN = 8;  // GP8, pin 11 (left barrier)
constexpr uint16_t ENTRY_OPEN_US = 500;
constexpr uint16_t ENTRY_CLOSED_US = 2000;

constexpr uint8_t EXIT_SERVO_PIN = 9;  // GP9, pin 12 (right barrier)
constexpr uint16_t EXIT_OPEN_US = 1000;
constexpr uint16_t EXIT_CLOSED_US = 2000;

// ---------------------------------------------------------------------------
// Kennzeichen: canton + 1..6 digits, no leading zero, e.g. "LU 123456".
// normalizePlate() uppercases and drops spaces ("lu 123 456" -> "LU123456").
// Returns an empty string if the plate is invalid.
// ---------------------------------------------------------------------------
inline String normalizePlate(const String& raw) {
    static const char* const CANTONS[] = {
        "AG", "AI", "AR", "BE", "BL", "BS", "FR", "GE", "GL", "GR", "JU", "LU", "NE",
        "NW", "OW", "SG", "SH", "SO", "SZ", "TG", "TI", "UR", "VD", "VS", "ZG", "ZH"};

    String p;
    for (char c : raw) {
        if (c != ' ') {
            p += static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }
    }
    if (p.length() < 3 || p.length() > 8) {
        return "";
    }
    bool cantonOk = false;
    for (const char* c : CANTONS) {
        if (p.startsWith(c)) {
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

// "LU123456" -> "LU 123456"
inline String displayPlate(const String& p) { return p.substring(0, 2) + " " + p.substring(2); }

inline String formatChf(uint32_t rappen) {
    char buf[16];
    snprintf(buf, sizeof(buf), "CHF %lu.%02lu", static_cast<unsigned long>(rappen / 100),
             static_cast<unsigned long>(rappen % 100));
    return buf;
}

inline String formatDuration(uint32_t minutes) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu h %02lu min", static_cast<unsigned long>(minutes / 60),
             static_cast<unsigned long>(minutes % 60));
    return buf;
}

// ---------------------------------------------------------------------------
// Date/time as seconds since 1970-01-01 (no time zone, the clock is set by hand).
// ---------------------------------------------------------------------------
inline int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

inline uint32_t makeTime(int32_t y, uint32_t mo, uint32_t d, uint32_t h, uint32_t mi) {
    return static_cast<uint32_t>(daysFromCivil(y, mo, d)) * 86400 + h * 3600 + mi * 60;
}

// -> "23.09.2026 08:00"
inline String formatDateTime(uint32_t t) {
    int32_t z = static_cast<int32_t>(t / 86400) + 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
    const int32_t y = static_cast<int32_t>(yoe) + era * 400 + (m <= 2);
    const uint32_t secs = t % 86400;

    char buf[40];
    snprintf(buf, sizeof(buf), "%02lu.%02lu.%04ld %02lu:%02lu", static_cast<unsigned long>(d),
             static_cast<unsigned long>(m), static_cast<long>(y),
             static_cast<unsigned long>(secs / 3600), static_cast<unsigned long>(secs / 60 % 60));
    return buf;
}

// Same format as <input type=datetime-local>: "2026-09-23T08:00"
inline String formatDateTimeInput(uint32_t t) {
    const String s = formatDateTime(t);  // "23.09.2026 08:00"
    return s.substring(6, 10) + "-" + s.substring(3, 5) + "-" + s.substring(0, 2) + "T" +
           s.substring(11, 16);
}

// Parses "2026-09-23T08:00". Returns 0 if the text is not a valid date/time.
inline uint32_t parseDateTimeInput(const String& s) {
    if (s.length() < 16 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':') {
        return 0;
    }
    const long y = s.substring(0, 4).toInt();
    const long mo = s.substring(5, 7).toInt();
    const long d = s.substring(8, 10).toInt();
    const long h = s.substring(11, 13).toInt();
    const long mi = s.substring(14, 16).toInt();
    if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59) {
        return 0;
    }
    return makeTime(y, mo, d, h, mi);
}

// ---------------------------------------------------------------------------
// Web helpers
// ---------------------------------------------------------------------------
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

static const char PAGE_STYLE[] PROGMEM = R"CSS(
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{font-family:system-ui,sans-serif;max-width:900px;margin:2em auto;padding:0 1em;color:#222}
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
