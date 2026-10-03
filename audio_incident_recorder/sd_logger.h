// sd_logger.h
// Логирование на microSD: CSV с событиями, WAV-фрагменты каждого события
// (для последующей ручной проверки/переразметки), диагностический лог.
#pragma once
#include "event_types.h"

namespace SdLogger {

// Инициализация SD (использует ту же SPI-шину, что и TFT — см. config.h),
// создаёт /events.csv (с заголовком, если файла ещё нет) и папку /events.
bool begin();

// Сохранить сырой фрагмент события как WAV-файл в /events/. outPath получает
// путь вида "/events/20260904_153012.wav". Возвращает false, если запись не
// удалась (например, SD не смонтирована или переполнена) — событие всё равно
// должно попасть в CSV-лог.
bool saveWav(const int16_t *samples, size_t numSamples, time_t startEpoch,
             char *outPath, size_t outPathSize);

// Добавить строку в /events.csv по итоговой записи о событии.
bool appendEventCsv(const EventRecord &ev);

// Короткая диагностическая строка с меткой времени в /diag.log (heap,
// вотчдог, статус Wi-Fi и т.п. — см. watchdog_diag.cpp).
bool appendDiagLine(const char *line);

// true, если SD успешно смонтирована и логирование доступно.
bool isAvailable();

} // namespace SdLogger
