// goertzel.h
// Лёгкие DSP-утилиты на алгоритме Гёрцеля: узкополосная оценка энергии на
// заданной частоте по кадру сэмплов. Дешевле полного FFT, и ровно то, что
// нужно для отслеживания тона сирены и грубой оценки спектра хлопка по полосам.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Один "бин" Гёрцеля, настроенный на конкретную частоту при заданных
// SAMPLE_RATE/N (N = число сэмплов в кадре, на котором считаем).
struct GoertzelBin {
    float freqHz;
    float coeff;
};

// Подготовить бин: посчитать коэффициент для частоты freqHz при данных
// sampleRate и N (длина кадра в сэмплах, ровно столько же сэмплов должно
// подаваться в goertzelPower()).
void goertzelPrepare(GoertzelBin &bin, float freqHz, float sampleRate, int n);

// Мощность (энергия, не в дБ) сигнала на частоте бина за кадр из n сэмплов.
// samples — int16 PCM.
float goertzelPower(const int16_t *samples, int n, const GoertzelBin &bin);

// RMS-энергия кадра (широкополосная, для порога громкости и для расчёта
// "тональности" = узкополосная/широкополосная энергия).
float frameRms(const int16_t *samples, int n);

// Пиковая абсолютная амплитуда кадра (для оценки фронта нарастания и пиковой
// громкости события).
int16_t framePeakAbs(const int16_t *samples, int n);

// Zero-crossing rate кадра (пересечений нуля / сэмпл), полезно для отличения
// "звонкого"/высокочастотного (металл, выстрел) от низкочастотного гула
// (гром, взрыв).
float frameZcr(const int16_t *samples, int n);
