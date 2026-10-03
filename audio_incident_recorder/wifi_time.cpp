#include "wifi_time.h"
#include "config.h"

#include <WiFi.h>
#include <time.h>

namespace WifiTime {

namespace {
bool s_timeSynced = false;
uint32_t s_lastReconnectAttemptMs = 0;
const uint32_t RECONNECT_INTERVAL_MS = 15000;
}

void begin() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[wifi] подключение");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] подключено, IP=%s\n", WiFi.localIP().toString().c_str());
        configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);

        struct tm tmInfo;
        if (getLocalTime(&tmInfo, 10000)) {
            s_timeSynced = true;
            Serial.println("[wifi] время синхронизировано по NTP");
        } else {
            Serial.println("[wifi] не удалось получить время по NTP при старте, попробуем позже");
        }
    } else {
        Serial.println("[wifi] не удалось подключиться при старте, будем пытаться в фоне");
    }
}

void poll() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!s_timeSynced) {
            struct tm tmInfo;
            if (getLocalTime(&tmInfo, 100)) {
                s_timeSynced = true;
                Serial.println("[wifi] время синхронизировано по NTP");
            }
        }
        return;
    }

    uint32_t now = millis();
    if (now - s_lastReconnectAttemptMs < RECONNECT_INTERVAL_MS) return;
    s_lastReconnectAttemptMs = now;

    Serial.println("[wifi] переподключение...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool isConnected() { return WiFi.status() == WL_CONNECTED; }
bool isTimeSynced() { return s_timeSynced; }

} // namespace WifiTime
