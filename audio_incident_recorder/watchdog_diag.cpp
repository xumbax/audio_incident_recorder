#include "watchdog_diag.h"
#include "config.h"
#include "sd_logger.h"
#include "wifi_time.h"

#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <stdio.h>

namespace WatchdogDiag {

namespace {
uint32_t s_lastHeapLogMs = 0;

// RTC_NOINIT_ATTR — память, которая переживает программный резет/панику
// (в отличие от обычных глобальных переменных), но НЕ инициализируется
// заново при каждой перезагрузке и содержит мусор после полного отключения
// питания — отсюда сигнатура s_magic, чтобы отличить "первое включение" от
// "перезагрузка после паники, счётчик ещё в силе".
RTC_NOINIT_ATTR uint32_t s_magic;
RTC_NOINIT_ATTR uint32_t s_crashStreak;
const uint32_t RTC_MAGIC_VALUE = 0x41524331; // "ARC1"
const uint32_t CRASH_STREAK_THRESHOLD = 3;   // столько паник подряд -> безопасный режим

bool s_safeModeDecided = false;
bool s_safeMode = false;
}

void begin() {
    // Сигнатура esp_task_wdt_init() зависит от версии ESP-IDF, на которой
    // собран ваш arduino-esp32 core (Arduino IDE её выбирает сама при
    // установке платформы esp32 — core 2.0.x -> IDF4.4, core 3.x -> IDF5).
    // Тестировалось на обеих веткам: core 2.0.9 (сборка в этом проекте) и
    // core 3.3.10 (то, что реально стоит у пользователя на Windows) —
    // компилятор сам выбирает нужную ветку ниже по ESP_IDF_VERSION_MAJOR,
    // руками ничего переключать не нужно.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdtConfig = {};
    wdtConfig.timeout_ms = (uint32_t)WDT_TIMEOUT_SEC * 1000UL;
    wdtConfig.idle_core_mask = 0; // не трогаем вотчдог "холостых" задач ядер
    wdtConfig.trigger_panic = true;
    esp_err_t err = esp_task_wdt_init(&wdtConfig);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[watchdog] esp_task_wdt_init failed: %d\n", err);
    }
    // ESP_ERR_INVALID_STATE здесь не ошибка: на core 3.x/IDF5 сам
    // arduino-esp32 уже запускает task watchdog на старте (до setup()) —
    // просто добавляем в него свою задачу ниже, таймаут в этом случае
    // берётся из конфигурации самого ядра, а не из WDT_TIMEOUT_SEC.
#else
    esp_task_wdt_init(WDT_TIMEOUT_SEC, true); // старая сигнатура IDF4.4: (timeout_s, panic)
#endif
    esp_task_wdt_add(nullptr); // текущая задача (обычно вызывается из setup() в контексте loopTask)
}

void feed() {
    esp_task_wdt_reset();
}

void poll() {
    uint32_t now = millis();
    if (now - s_lastHeapLogMs < HEAP_LOG_INTERVAL_MS) return;
    s_lastHeapLogMs = now;

    uint32_t freeHeap = esp_get_free_heap_size();
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    uint32_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    char line[160];
    snprintf(line, sizeof(line),
              "uptime=%lus heap_free=%lu heap_largest_block=%lu psram_free=%lu wifi=%s time_sync=%s",
              (unsigned long)(now / 1000), (unsigned long)freeHeap, (unsigned long)largestBlock,
              (unsigned long)freePsram,
              WifiTime::isConnected() ? "up" : "down",
              WifiTime::isTimeSynced() ? "yes" : "no");

    Serial.print("[diag] ");
    Serial.println(line);
    SdLogger::appendDiagLine(line);
}

bool shouldSkipRiskyInit() {
    if (s_safeModeDecided) return s_safeMode; // решение на эту загрузку уже принято
    s_safeModeDecided = true;

    if (s_magic != RTC_MAGIC_VALUE) {
        // RTC-память после включения питания содержит мусор — это первая
        // инициализация нашего счётчика, начинаем с нуля.
        s_magic = RTC_MAGIC_VALUE;
        s_crashStreak = 0;
    }

    esp_reset_reason_t reason = esp_reset_reason();
    bool crashLike = (reason == ESP_RST_PANIC) || (reason == ESP_RST_INT_WDT) ||
                      (reason == ESP_RST_TASK_WDT) || (reason == ESP_RST_WDT);

    if (crashLike) {
        s_crashStreak++;
    } else {
        s_crashStreak = 0; // обычный старт (питание/кнопка/сброс из IDE) — серия не в счёт
    }

    Serial.printf("[watchdog] причина перезагрузки: %d, падений подряд: %lu\n",
                   (int)reason, (unsigned long)s_crashStreak);

    if (s_crashStreak >= CRASH_STREAK_THRESHOLD) {
        Serial.println("[watchdog] несколько падений подряд сразу после старта — "
                        "включаем БЕЗОПАСНЫЙ РЕЖИМ (без экрана), чтобы устройство "
                        "хотя бы слушало и писало на SD, а не падало молча по кругу");
        s_safeMode = true;
    }

    return s_safeMode;
}

void markBootStable() {
    if (s_crashStreak != 0) {
        Serial.println("[watchdog] запуск стабилен уже некоторое время — сбрасываем счётчик падений");
    }
    s_crashStreak = 0;
}

} // namespace WatchdogDiag
