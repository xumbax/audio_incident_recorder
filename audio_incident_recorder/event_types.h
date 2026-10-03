// event_types.h
// Общие структуры данных, которыми обмениваются детектор событий,
// анализатор сирены, классификатор хлопков, лог на SD и экран.
#pragma once
#include <Arduino.h>
#include <time.h>
#include "config.h"
#include "direction.h"

enum class EventKind : uint8_t {
    SIREN,       // тональный сигнал (сирена)
    BANG,        // короткий импульсный звук (хлопок/удар)
    LONG_NOISE   // громкий продолжительный звук без тональности и без
                 // характерного импульсного профиля — не смогли уверенно
                 // отнести ни к сирене, ни к хлопку
};

// --- Сирена -----------------------------------------------------------

struct SirenResult {
    int   segmentCount = 0;                       // сколько отрезков тона зафиксировано
    float segmentSec[MAX_SIREN_SEGMENTS] = {0};    // длительность каждого отрезка, сек
    float gapSec[MAX_SIREN_SEGMENTS]     = {0};    // пауза ПОСЛЕ отрезка i (0 для последнего)
    float totalActiveSec = 0;                      // суммарная длительность звучания (без пауз)
    bool  truncated = false;                       // отрезков было больше, чем поместилось
    char  patternText[96] = {0};                   // готовое текстовое описание паттерна
};

// --- Хлопок -------------------------------------------------------------

enum class BangCategory : uint8_t {
    THUNDER,     // похоже на грозу/гром
    GUNSHOT,     // похоже на выстрел
    EXPLOSION,   // похоже на взрыв
    METAL,       // похоже на удар по металлу
    UNKNOWN      // не удалось уверенно классифицировать
};

struct BangFeatures {
    float   attackMs   = 0;   // время фронта нарастания 10%->90% пика
    float   durationMs = 0;   // длительность события над порогом
    float   decayMs    = 0;   // время спада от пика до возврата к порогу
    float   lowRatio   = 0;   // доля энергии в низкой полосе (<300 Гц)
    float   midRatio   = 0;   // доля энергии в средней полосе (300-2000 Гц)
    float   highRatio  = 0;   // доля энергии в высокой полосе (>2000 Гц)
    float   tailHighRatio = 0;// доля высокочастотной энергии в "хвосте" после фронта (признак звона металла)
    float   zcr        = 0;   // средний zero-crossing rate по событию
    int16_t peakAmp    = 0;   // пиковая амплитуда (int16, до 32767)
    bool    clipped    = false; // был ли клиппинг (капсюль микрофона перегружен)
    bool    doublePulse = false; // второй выраженный пик после первого (возможна вторая детонация)
};

struct BangResult {
    BangCategory category = BangCategory::UNKNOWN;
    float        confidence = 0;   // эвристическая оценка уверенности, 0..1
    BangFeatures features;
};

// --- Итоговая запись о событии ------------------------------------------

struct EventRecord {
    EventKind kind;
    time_t    startEpoch  = 0;   // время начала события (unix time, из NTP)
    float     durationSec = 0;
    int16_t   peakAmp     = 0;
    float     peakDbfs    = 0;   // громкость: пик за событие, дБ отн. полной шкалы (см. loudness.h)
    float     avgDbfs     = 0;   // громкость: средняя (по RMS) за событие, дБFS
    Direction::Side directionSide = Direction::Side::UNKNOWN; // снизу/сверху/по центру (см. direction.h)
    float     directionDelayMs = 0;    // задержка нижний/верхний мик, мс (диагностика/калибровка)
    float     directionConfidence = 0; // 0..1, уверенность оценки угла места
    SirenResult siren;           // заполнено, если kind == SIREN
    BangResult  bang;            // заполнено, если kind == BANG или LONG_NOISE
    char       wavPath[64] = {0}; // путь к сохранённому WAV-фрагменту, пусто если не сохранён
};

const char *bangCategoryLabel(BangCategory c);
const char *eventKindLabel(EventKind k);
