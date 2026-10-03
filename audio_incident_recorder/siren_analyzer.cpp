#include "siren_analyzer.h"
#include "goertzel.h"
#include "config.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace SirenAnalyzer {

namespace {

GoertzelBin s_bandBins[SIREN_BAND_BINS];
GoertzelBin s_refBins[2]; // опорные бины вне полосы сирены (шумовой фон)
bool s_ready = false;

void ensureReady() {
    if (s_ready) return;
    for (int i = 0; i < SIREN_BAND_BINS; i++) {
        float f = SIREN_BAND_LOW_HZ +
                  (SIREN_BAND_HIGH_HZ - SIREN_BAND_LOW_HZ) * i / (float)(SIREN_BAND_BINS - 1);
        goertzelPrepare(s_bandBins[i], f, SAMPLE_RATE_HZ, FRAME_SAMPLES);
    }
    goertzelPrepare(s_refBins[0], 200.0f, SAMPLE_RATE_HZ, FRAME_SAMPLES);
    goertzelPrepare(s_refBins[1], 3000.0f, SAMPLE_RATE_HZ, FRAME_SAMPLES);
    s_ready = true;
}

} // namespace

float toneRatio(const int16_t *frame, int n) {
    ensureReady();

    float bandPeak = 0;
    for (int i = 0; i < SIREN_BAND_BINS; i++) {
        float p = goertzelPower(frame, n, s_bandBins[i]);
        if (p > bandPeak) bandPeak = p;
    }

    float refAvg = (goertzelPower(frame, n, s_refBins[0]) +
                     goertzelPower(frame, n, s_refBins[1])) * 0.5f;

    return bandPeak / (refAvg + 1.0f); // +1 — просто чтобы не делить на почти-ноль в тишине
}

void finalize(SirenResult &result) {
    if (result.segmentCount <= 0) {
        strncpy(result.patternText, "нет данных", sizeof(result.patternText) - 1);
        return;
    }

    float sum = 0, minSeg = result.segmentSec[0], maxSeg = result.segmentSec[0];
    for (int i = 0; i < result.segmentCount; i++) {
        sum += result.segmentSec[i];
        if (result.segmentSec[i] < minSeg) minSeg = result.segmentSec[i];
        if (result.segmentSec[i] > maxSeg) maxSeg = result.segmentSec[i];
    }
    result.totalActiveSec = sum;

    char *p = result.patternText;
    size_t cap = sizeof(result.patternText);

    if (result.segmentCount == 1) {
        snprintf(p, cap, "непрерывная %.0f с", result.segmentSec[0]);
        return;
    }

    // Похожи ли все отрезки по длительности (разброс <= 25% от среднего)?
    float avg = sum / result.segmentCount;
    bool uniform = true;
    for (int i = 0; i < result.segmentCount; i++) {
        if (fabsf(result.segmentSec[i] - avg) > 0.25f * avg + 0.3f) { uniform = false; break; }
    }

    if (uniform) {
        float avgGap = 0;
        int gapCount = result.segmentCount - 1;
        for (int i = 0; i < gapCount; i++) avgGap += result.gapSec[i];
        if (gapCount > 0) avgGap /= gapCount;
        // segmentCount <= MAX_SIREN_SEGMENTS (24) и avg/avgGap <= MAX_EVENT_SEC
        // (600) по построению — GCC этого не знает (float/int для него в
        // принципе неограничены) и считает "%.0f" худшим случаем на весь
        // диапазон double, отсюда ложное срабатывание -Wformat-truncation.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(p, cap, "%d отрезка(ов) по ~%.0f с (пауза ~%.0f с)%s",
                 result.segmentCount, avg, avgGap, result.truncated ? ", продолжалось дальше" : "");
#pragma GCC diagnostic pop
        return;
    }

    if (result.segmentCount == 2) {
        const char *first = result.segmentSec[0] < result.segmentSec[1] ? "короткая" : "длинная";
        const char *second = result.segmentSec[0] < result.segmentSec[1] ? "длинная" : "короткая";
        snprintf(p, cap, "%s %.0fс + %s %.0fс", first, result.segmentSec[0], second, result.segmentSec[1]);
        return;
    }

    // Общий случай: перечисляем отрезки через дефис, пока хватает места в буфере.
    int written = snprintf(p, cap, "%d отрезков: ", result.segmentCount);
    for (int i = 0; i < result.segmentCount && written < (int)cap - 1; i++) {
        int n = snprintf(p + written, cap - written, i == 0 ? "%.0fс" : "-%.0fс", result.segmentSec[i]);
        if (n < 0) break;
        written += n;
    }
    if (result.truncated && written < (int)cap - 4) {
        snprintf(p + written, cap - written, "...");
    }
}

} // namespace SirenAnalyzer
