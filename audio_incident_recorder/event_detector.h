// event_detector.h
// Центральная логика: непрерывно читает кадры из AudioCapture, ведёт
// детектор громкости с адаптивным порогом, решает "сирена / хлопок / шум",
// собирает сегменты паттерна сирены, вызывает классификатор хлопков,
// пишет на SD и отдаёт готовые события наружу (для экрана/логики main-файла).
//
// Работает как отдельная FreeRTOS-задача (см. begin()). Все обращения снаружи
// — через потокобезопасные getLiveStatus()/popFinishedEvent().
#pragma once
#include "event_types.h"
#include "direction.h"

namespace EventDetector {

bool begin();

struct LiveStatus {
    bool  inEvent = false;
    bool  isSirenCandidate = false; // уже подтверждено как тон/сирена
    float elapsedSec = 0;
    float peakDbfs = 0; // громкость по факту "на данный момент" (см. loudness.h)
    Direction::Side directionSide = Direction::Side::UNKNOWN; // снизу/сверху, оценено в момент старта события (см. direction.h)
};

LiveStatus getLiveStatus();

// Если с прошлого вызова появилось новое завершённое событие — скопировать
// его в out и вернуть true (один раз, повторный вызов до следующего события
// вернёт false).
bool popFinishedEvent(EventRecord &out);

} // namespace EventDetector
