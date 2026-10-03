// Регистратор аудио-инцидентов (сирены/хлопки)
// ESP32-S3 N16R8 + 2x INMP441 (I2S, стерео, микрофоны смонтированы
// ВЕРТИКАЛЬНО — второй мик выше на мачте, для грубой оценки угла места
// снизу/сверху) + ILI9341 240x320 (SPI) + microSD (SPI)
//
// Общая идея и разбор компонентов — см. проект narodmon,
// claude/audio-incident-recorder-design.md.
//
// Перед первой прошивкой:
//   1. Впишите свои WIFI_SSID / WIFI_PASSWORD в config.h
//   2. Сверьте пины в config.h с тем, что реально распаяно на вашей плате
//   3. Настройте TFT_eSPI/User_Setup.h так, чтобы пины TFT совпадали с config.h
//      (см. README.md)
//   4. В Arduino IDE: Tools -> PSRAM: "OPI PSRAM" (обязательно, иначе буферы
//      захвата не выделятся и детектор откажется стартовать)

#include "config.h"
#include "event_types.h"
#include "audio_capture.h"
#include "event_detector.h"
#include "display_ui.h"
#include "wifi_time.h"
#include "watchdog_diag.h"
#include "sd_logger.h"
#include "direction.h"

#include <SPI.h>

// Через сколько мс работы без падения считаем этот запуск "стабильным" и
// сбрасываем счётчик серии падений (см. watchdog_diag.h) — чтобы одна
// случайная перезагрузка через день работы не включала безопасный режим
// сразу же после следующей.
static const uint32_t STABLE_UPTIME_MS = 20000;
static bool s_bootMarkedStable = false;

// Печатает контрольную точку в Serial И кормит вотчдог — расставлено между
// КАЖДЫМ шагом setup(). Если устройство снова уйдёт в Guru Meditation, по
// последней увиденной в Serial контрольной точке сразу видно, какой именно
// шаг инициализации не пережил конкретный экземпляр железа — без разбора
// backtrace через addr2line.
static void checkpoint(const char *label) {
    Serial.printf("[main] checkpoint: %s\n", label);
    WatchdogDiag::feed();
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Регистратор аудио-инцидентов: старт ===");

    // Вотчдог поднимаем МАКСИМАЛЬНО рано (до любых потенциально рискованных
    // вызовов ниже) — если что-то из последующего надолго зависнет (а не
    // сразу упадёт), сторожевой таймер перезагрузит устройство сам, а не
    // оставит его висеть немым кирпичом.
    WatchdogDiag::begin();
    checkpoint("WatchdogDiag::begin() выполнен");

    // Смотрим, не падали ли предыдущие несколько загрузок подряд сразу
    // после старта (см. watchdog_diag.h) — если да, программно отключаем
    // экран ниже и работаем без него, вместо повторного падения по кругу.
    bool safeMode = WatchdogDiag::shouldSkipRiskyInit();
    if (safeMode) {
        Serial.println("[main] БЕЗОПАСНЫЙ РЕЖИМ: экран отключён на этой загрузке (см. причину выше)");
    }

    // Проверка PSRAM ДО того, как она реально понадобится (event_detector/
    // direction выделяют из неё буферы) — если выключена/не распознана,
    // печатаем понятную причину сразу, а не ждём до EventDetector::begin().
    if (ESP.getPsramSize() == 0) {
        Serial.println("[main] ВНИМАНИЕ: PSRAM не обнаружена (0 байт) — проверьте Tools -> PSRAM: "
                        "\"OPI PSRAM\" в Arduino IDE. Без этого EventDetector::begin() ниже не сможет "
                        "выделить буферы захвата и откажется стартовать.");
    } else {
        Serial.printf("[main] PSRAM обнаружена: %u байт\n", (unsigned)ESP.getPsramSize());
    }
    checkpoint("проверка PSRAM выполнена");

    // Общую SPI-шину для TFT и SD (см. config.h — SD сидит на своём CS на
    // той же шине) поднимает DisplayUI::begin() ниже (через tft.init(),
    // либо сама, если экран отключён безопасным режимом) — НЕ здесь.
    // Раньше SPI.begin() вызывался и тут, и (повторно, с другим SS-пином)
    // внутри tft.init() — именно это на реальном железе роняло устройство в
    // Guru Meditation Error сразу после старта, см. подробности в
    // display_ui.cpp и README ("Устойчивость к сбоям").
    DisplayUI::setEnabled(!safeMode);
    DisplayUI::begin();
    checkpoint("DisplayUI::begin() выполнен");
    DisplayUI::setStatusLine("старт...");

    WifiTime::begin();
    checkpoint("WifiTime::begin() выполнен");

    if (!SdLogger::begin()) {
        Serial.println("[main] SD недоступна — события будут видны на экране/в Serial, но НЕ будут сохраняться");
        DisplayUI::setStatusLine("SD не найдена!");
        delay(2000);
    }
    checkpoint("SdLogger::begin() выполнен");

    if (!AudioCapture::begin()) {
        DisplayUI::setStatusLine("ошибка микрофона!");
        Serial.println("[main] AudioCapture::begin() не удался — проверьте пины I2S и питание INMP441");
        // Не зависаем немо: кормим вотчдог (иначе через WDT_TIMEOUT_SEC
        // получим ещё одну панику поверх уже понятной причины) и повторяем
        // диагностическое сообщение, чтобы оно было видно, даже если
        // подключиться к Serial успели не с самого начала.
        while (true) {
            WatchdogDiag::feed();
            delay(1000);
            static uint8_t n = 0;
            if (++n >= 5) { // раз в 5с
                n = 0;
                Serial.println("[main] ...жду: AudioCapture::begin() не удался, проверьте микрофоны");
            }
        }
    }
    checkpoint("AudioCapture::begin() выполнен");

    if (!EventDetector::begin()) {
        DisplayUI::setStatusLine("ошибка детектора!");
        Serial.println("[main] EventDetector::begin() не удался — вероятно, не хватило PSRAM (включена ли OPI PSRAM в Tools?)");
        while (true) {
            WatchdogDiag::feed();
            delay(1000);
            static uint8_t n = 0;
            if (++n >= 5) {
                n = 0;
                Serial.println("[main] ...жду: EventDetector::begin() не удался, проверьте PSRAM (Tools -> PSRAM: OPI PSRAM)");
            }
        }
    }
    checkpoint("EventDetector::begin() выполнен");

    DisplayUI::setStatusLine("прослушивание...");
    Serial.println("[main] инициализация завершена, слушаем");
}

void loop() {
    WifiTime::poll();
    WatchdogDiag::poll();
    WatchdogDiag::feed();
    DisplayUI::tick();

    if (!s_bootMarkedStable && millis() >= STABLE_UPTIME_MS) {
        s_bootMarkedStable = true;
        WatchdogDiag::markBootStable();
    }

    EventRecord ev;
    if (EventDetector::popFinishedEvent(ev)) {
        DisplayUI::pushFinishedEvent(ev);

        if (ev.kind == EventKind::SIREN) {
            Serial.printf("[event] SIREN dur=%.1fs peak=%d (%.1f/%.1f dBFS peak/avg) dir=%s(%.2fms,conf=%.2f) pattern=\"%s\" wav=%s\n",
                           ev.durationSec, ev.peakAmp, ev.peakDbfs, ev.avgDbfs,
                           Direction::sideLabel(ev.directionSide), ev.directionDelayMs, ev.directionConfidence,
                           ev.siren.patternText, ev.wavPath);
        } else {
            Serial.printf("[event] %s: %s dur=%.1fs peak=%d (%.1f/%.1f dBFS peak/avg) dir=%s(%.2fms,conf=%.2f) conf=%.2f wav=%s\n",
                           eventKindLabel(ev.kind), bangCategoryLabel(ev.bang.category),
                           ev.durationSec, ev.peakAmp, ev.peakDbfs, ev.avgDbfs,
                           Direction::sideLabel(ev.directionSide), ev.directionDelayMs, ev.directionConfidence,
                           ev.bang.confidence, ev.wavPath);
        }
    } else {
        EventDetector::LiveStatus live = EventDetector::getLiveStatus();
        if (live.inEvent) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s %.0fс %.0fдБ %.4s...",
                      live.isSirenCandidate ? "СИРЕНА" : "событие", live.elapsedSec, live.peakDbfs,
                      Direction::sideShortLabel(live.directionSide));
            DisplayUI::setStatusLine(buf);
        }
    }

    delay(100);
}
