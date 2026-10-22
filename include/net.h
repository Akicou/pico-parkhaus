#pragma once

#include <Arduino.h>
#include <SimpleMDNS.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#include "config.h"

// Joins one of WIFI_NETWORKS and announces <hostname>.local via mDNS.
// loop() reconnects if the hotspot was gone. The onboard LED shows the connection.
class Net {
public:
    void begin(const char* hostname) {
        _hostname = hostname;
        pinMode(LED_BUILTIN, OUTPUT);
        WiFi.setHostname(hostname);
        for (const WifiNetwork& n : WIFI_NETWORKS) {
            _multi.addAP(n.ssid, n.pass);
        }
        connect();
    }

    void loop() {
        MDNS.update();
        if (millis() - _lastCheck < 10000) {
            return;
        }
        _lastCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            digitalWrite(LED_BUILTIN, LOW);
            connect();
        } else {
            Serial.printf("%s: http://%s/ (http://%s.local/) im WLAN %s\n", _hostname,
                          WiFi.localIP().toString().c_str(), _hostname, WiFi.SSID().c_str());
        }
    }

private:
    void connect() {
        Serial.println("Verbinde mit WLAN ...");
        if (_multi.run(15000) == WL_CONNECTED) {
            digitalWrite(LED_BUILTIN, HIGH);
            if (!_mdnsStarted) {
                _mdnsStarted = MDNS.begin(_hostname);
            }
            Serial.printf("Verbunden mit %s, IP %s\n", WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str());
        } else {
            Serial.println("Kein WLAN gefunden, neuer Versuch in 5 s");
        }
    }

    WiFiMulti _multi;
    const char* _hostname = "";
    uint32_t _lastCheck = 0;
    bool _mdnsStarted = false;
};
