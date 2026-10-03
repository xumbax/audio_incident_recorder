// siren_analyzer.h
// Обнаружение тона в полосе сирены (для решения "это сирена, а не хлопок/шум")
// и построение человекочитаемого описания паттерна по накопленным сегментам
// вкл/выкл, которые собирает event_detector.
#pragma once
#include "event_types.h"

namespace SirenAnalyzer {

// Насколько кадр "тональный и в полосе сирены": отношение мощности самого
// сильного бина в полосе SIREN_BAND_LOW_HZ..SIREN_BAND_HIGH_HZ к мощности
// пары опорных бинов вне полосы (той же природы вычисления — Гёрцель, так
// что единицы измерения совпадают и результат — безразмерное отношение).
// >= TONE_RATIO_THRESHOLD из config.h считаем тональным кадром.
float toneRatio(const int16_t *frame, int n);

// Собрать patternText и totalActiveSec в result по заполненным
// segmentSec/gapSec/segmentCount.
void finalize(SirenResult &result);

} // namespace SirenAnalyzer
