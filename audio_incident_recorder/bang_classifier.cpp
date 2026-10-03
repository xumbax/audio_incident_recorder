#include "bang_classifier.h"
#include "goertzel.h"
#include "config.h"
#include <math.h>
#include <string.h>

namespace {

// Частоты для оценки энергии по трём широким полосам. Несколько бинов на
// полосу — просто чтобы не зависеть от одной точки спектра.
const float LOW_BAND_HZ[]  = {80, 150, 220};
const float MID_BAND_HZ[]  = {400, 800, 1200, 1800};
const float HIGH_BAND_HZ[] = {2500, 3500, 5000, 7000};

// Максимум кадров анализа на одно событие (совпадает с CAPTURE_MAX_SAMPLES).
const int MAX_FRAMES = (int)(CAPTURE_MAX_SAMPLES / FRAME_SAMPLES) + 1;

struct BandBins {
    GoertzelBin low[sizeof(LOW_BAND_HZ) / sizeof(float)];
    GoertzelBin mid[sizeof(MID_BAND_HZ) / sizeof(float)];
    GoertzelBin high[sizeof(HIGH_BAND_HZ) / sizeof(float)];
    bool ready = false;
};

BandBins s_bins;

void ensureBinsReady() {
    if (s_bins.ready) return;
    for (size_t i = 0; i < sizeof(LOW_BAND_HZ) / sizeof(float); i++)
        goertzelPrepare(s_bins.low[i], LOW_BAND_HZ[i], SAMPLE_RATE_HZ, FRAME_SAMPLES);
    for (size_t i = 0; i < sizeof(MID_BAND_HZ) / sizeof(float); i++)
        goertzelPrepare(s_bins.mid[i], MID_BAND_HZ[i], SAMPLE_RATE_HZ, FRAME_SAMPLES);
    for (size_t i = 0; i < sizeof(HIGH_BAND_HZ) / sizeof(float); i++)
        goertzelPrepare(s_bins.high[i], HIGH_BAND_HZ[i], SAMPLE_RATE_HZ, FRAME_SAMPLES);
    s_bins.ready = true;
}

float bandEnergy(const int16_t *frame, int n, const GoertzelBin *bins, size_t count) {
    float sum = 0;
    for (size_t i = 0; i < count; i++) sum += goertzelPower(frame, n, bins[i]);
    return sum / (float)count;
}

} // namespace

BangResult classifyBang(const int16_t *samples, size_t numSamples) {
    ensureBinsReady();

    BangResult result;
    BangFeatures &f = result.features;

    int frameCount = (int)(numSamples / FRAME_SAMPLES);
    if (frameCount < 2) {
        // Событие слишком короткое для содержательного анализа — отдаём как
        // есть, UNKNOWN.
        return result;
    }
    if (frameCount > MAX_FRAMES) frameCount = MAX_FRAMES;

    static float envelope[MAX_FRAMES];
    static float lowE[MAX_FRAMES], midE[MAX_FRAMES], highE[MAX_FRAMES];
    static float zcrArr[MAX_FRAMES];

    int16_t peakAmp = 0;
    int peakIdx = 0;

    for (int i = 0; i < frameCount; i++) {
        const int16_t *frame = samples + (size_t)i * FRAME_SAMPLES;
        envelope[i] = frameRms(frame, FRAME_SAMPLES);
        lowE[i]  = bandEnergy(frame, FRAME_SAMPLES, s_bins.low,  sizeof(LOW_BAND_HZ)/sizeof(float));
        midE[i]  = bandEnergy(frame, FRAME_SAMPLES, s_bins.mid,  sizeof(MID_BAND_HZ)/sizeof(float));
        highE[i] = bandEnergy(frame, FRAME_SAMPLES, s_bins.high, sizeof(HIGH_BAND_HZ)/sizeof(float));
        zcrArr[i] = frameZcr(frame, FRAME_SAMPLES);

        int16_t p = framePeakAbs(frame, FRAME_SAMPLES);
        if (p > peakAmp) { peakAmp = p; peakIdx = i; }
    }

    f.peakAmp = peakAmp;
    f.clipped = peakAmp >= 32000;

    float peakEnv = envelope[peakIdx];
    if (peakEnv <= 0.0001f) peakEnv = 0.0001f;

    // --- Фронт нарастания (10% -> 90% пика) ---
    int idx10 = 0, idx90 = peakIdx;
    for (int i = 0; i <= peakIdx; i++) {
        if (envelope[i] >= 0.10f * peakEnv) { idx10 = i; break; }
    }
    for (int i = idx10; i <= peakIdx; i++) {
        if (envelope[i] >= 0.90f * peakEnv) { idx90 = i; break; }
    }
    f.attackMs = (float)(idx90 - idx10) * FRAME_MS;
    if (f.attackMs < FRAME_MS) f.attackMs = FRAME_MS;

    // --- Длительность и спад: границы, где огибающая выше 15% пика ---
    const float RELEASE_LEVEL = 0.15f;
    int firstAbove = idx10, lastAbove = peakIdx;
    for (int i = 0; i < frameCount; i++) {
        if (envelope[i] >= RELEASE_LEVEL * peakEnv) { firstAbove = i; break; }
    }
    for (int i = frameCount - 1; i >= peakIdx; i--) {
        if (envelope[i] >= RELEASE_LEVEL * peakEnv) { lastAbove = i; break; }
    }
    f.durationMs = (float)(lastAbove - firstAbove + 1) * FRAME_MS;
    f.decayMs = (float)(lastAbove - peakIdx + 1) * FRAME_MS;

    // --- Энергия по полосам, усреднённая по всему активному участку ---
    double lowSum = 0, midSum = 0, highSum = 0;
    double zcrSum = 0;
    int activeCount = 0;
    for (int i = firstAbove; i <= lastAbove; i++) {
        lowSum += lowE[i]; midSum += midE[i]; highSum += highE[i];
        zcrSum += zcrArr[i];
        activeCount++;
    }
    if (activeCount < 1) activeCount = 1;
    double bandTotal = lowSum + midSum + highSum;
    if (bandTotal < 1e-6) bandTotal = 1e-6;
    f.lowRatio  = (float)(lowSum  / bandTotal);
    f.midRatio  = (float)(midSum  / bandTotal);
    f.highRatio = (float)(highSum / bandTotal);
    f.zcr = (float)(zcrSum / activeCount);

    // --- "Хвост" после начального фронта: признак металлического звона ---
    int tailStart = peakIdx + (int)ceilf(150.0f / FRAME_MS);
    if (tailStart <= peakIdx) tailStart = peakIdx + 1;
    if (tailStart <= lastAbove) {
        double tailLow = 0, tailMid = 0, tailHigh = 0;
        for (int i = tailStart; i <= lastAbove; i++) {
            tailLow += lowE[i]; tailMid += midE[i]; tailHigh += highE[i];
        }
        double tailTotal = tailLow + tailMid + tailHigh;
        if (tailTotal < 1e-6) tailTotal = 1e-6;
        f.tailHighRatio = (float)(tailHigh / tailTotal);
    } else {
        f.tailHighRatio = 0; // событие закончилось раньше, чем начался "хвост"
    }

    // --- Второй выраженный импульс (возможна вторая детонация) ---
    f.doublePulse = false;
    int refractoryFrames = (int)ceilf(30.0f / FRAME_MS);
    for (int i = peakIdx + refractoryFrames + 1; i < lastAbove; i++) {
        bool localMax = envelope[i] > envelope[i - 1] && envelope[i] >= envelope[i + 1 < frameCount ? i + 1 : i];
        if (localMax && envelope[i] > 0.40f * peakEnv) {
            f.doublePulse = true;
            break;
        }
    }

    // --- Взвешенная классификация (пороги ниже — TUNE ME по реальным записям) ---
    float scoreThunder = 0, scoreGunshot = 0, scoreExplosion = 0, scoreMetal = 0;

    if (f.attackMs < 15.0f)        { scoreGunshot += 2; scoreExplosion += 1; }
    else if (f.attackMs < 40.0f)   { scoreExplosion += 1; }
    else if (f.attackMs > 50.0f)   { scoreThunder += 2; }

    if (f.durationMs < 250.0f)     { scoreGunshot += 1; scoreMetal += 1; }
    else if (f.durationMs > 600.0f){ scoreThunder += 2; scoreExplosion += 1; }

    if (f.lowRatio > 0.50f)        { scoreExplosion += 2; scoreThunder += 1; }
    if (f.highRatio > 0.45f)       { scoreGunshot += 1; scoreMetal += 2; }
    if (f.tailHighRatio > 0.40f && f.highRatio > 0.25f) { scoreMetal += 3; }

    if (f.zcr > 0.25f)             { scoreGunshot += 1; scoreMetal += 1; }
    else if (f.zcr < 0.08f)        { scoreThunder += 1; scoreExplosion += 1; }

    if (f.clipped)                 { scoreExplosion += 1; }
    if (f.doublePulse)             { scoreExplosion += 2; }

    float scores[4] = {scoreThunder, scoreGunshot, scoreExplosion, scoreMetal};
    BangCategory cats[4] = {BangCategory::THUNDER, BangCategory::GUNSHOT,
                             BangCategory::EXPLOSION, BangCategory::METAL};

    int bestIdx = 0;
    float total = 0;
    for (int i = 0; i < 4; i++) {
        total += scores[i];
        if (scores[i] > scores[bestIdx]) bestIdx = i;
    }

    const float MIN_SCORE_TO_DECIDE = 2.0f;
    if (scores[bestIdx] < MIN_SCORE_TO_DECIDE) {
        result.category = BangCategory::UNKNOWN;
        result.confidence = total > 0 ? scores[bestIdx] / total : 0.0f;
    } else {
        result.category = cats[bestIdx];
        result.confidence = total > 0 ? scores[bestIdx] / total : 0.0f;
    }

    return result;
}
