// wifi_time.h
// Подключение к Wi-Fi и синхронизация времени по NTP. Питание и Wi-Fi на
// месте установки всегда доступны (см. audio-incident-recorder-design.md),
// поэтому RTC не используется — достаточно периодической NTP-синхронизации.
#pragma once

namespace WifiTime {

void begin();

// Вызывать периодически из loop() — неблокирующий переподключ при обрыве.
void poll();

bool isConnected();
bool isTimeSynced();

} // namespace WifiTime
