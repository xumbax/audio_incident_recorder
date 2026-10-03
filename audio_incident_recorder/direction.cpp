#include "direction.h"
#include "config.h"

#include <esp_heap_caps.h>
#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace Direction {

namespace {

int16_t *s_ringL = nullptr;
int16_t *s_ringR = nullptr;
size_t   s_ringWriteIdx = 0;
bool     s_ready = false;

// Максимальный физически возможный сдвиг между каналами при заданном
// MIC_SPACING_M (config.h) — дальше него пик искать бессмысленно (там его
// заведомо быть не может), и это заодно сильно сокращает объём вычислений.
// +2 сэмпла запаса на неточность MIC_SPACING_M/скорости звука.
const int MAX_LAG_SAMPLES =
    (int)ceilf(MIC_SPACING_M / SPEED_OF_SOUND_MPS * SAMPLE_RATE_HZ) + 2;

// Рабочие буферы для кросс-корреляции. Держим во внутренней SRAM (не PSRAM) —
// это горячий цикл (~DIRECTION_WINDOW_SAMPLES * 2*MAX_LAG_SAMPLES операций за
// один вызов estimate()), внутренняя память ощутимо быстрее внешней SPI PSRAM.
// Дают в сумме ~38КБ SRAM при DIRECTION_WINDOW_SEC=0.3 — на фоне ~320КБ на
// плате это не критично.
float s_windowL[DIRECTION_WINDOW_SAMPLES];
float s_windowR[DIRECTION_WINDOW_SAMPLES];

} // namespace

bool begin() {
    s_ringL = (int16_t *)heap_caps_malloc(PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_ringR = (int16_t *)heap_caps_malloc(PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_ringL || !s_ringR) {
        Serial.println("[direction] не удалось выделить буферы в PSRAM — оценка угла места (снизу/сверху) работать не будет, остальная детекция не затронута");
        return false;
    }
    memset(s_ringL, 0, PREROLL_SAMPLES * sizeof(int16_t));
    memset(s_ringR, 0, PREROLL_SAMPLES * sizeof(int16_t));
    s_ready = true;
    return true;
}

void pushFrame(const int16_t *left, const int16_t *right, int n) {
    if (!s_ready) return;
    for (int i = 0; i < n; i++) {
        s_ringL[s_ringWriteIdx] = left[i];
        s_ringR[s_ringWriteIdx] = right[i];
        s_ringWriteIdx++;
        if (s_ringWriteIdx >= PREROLL_SAMPLES) s_ringWriteIdx = 0;
    }
}

const char *sideLabel(Side s) {
    switch (s) {
        case Side::BELOW:  return "снизу";
        case Side::ABOVE:  return "сверху";
        case Side::CENTER: return "по центру";
        default:           return "?";
    }
}

const char *sideShortLabel(Side s) {
    switch (s) {
        case Side::BELOW:  return "Н";
        case Side::ABOVE:  return "В";
        case Side::CENTER: return "Ц";
        default:           return "?";
    }
}

Estimate estimate() {
    Estimate result;
    if (!s_ready) return result;

    // Последние DIRECTION_WINDOW_SAMPLES кольцевого буфера — самая свежая
    // история на момент вызова. Вызывается сразу при срабатывании детектора
    // (event_detector::startEvent()), пока фронт атаки события ещё не "уехал"
    // дальше конца окна.
    size_t start = (s_ringWriteIdx + PREROLL_SAMPLES - DIRECTION_WINDOW_SAMPLES) % PREROLL_SAMPLES;
    for (size_t i = 0; i < DIRECTION_WINDOW_SAMPLES; i++) {
        size_t idx = (start + i) % PREROLL_SAMPLES;
        s_windowL[i] = (float)s_ringL[idx];
        s_windowR[i] = (float)s_ringR[idx];
    }

    // Нормированная кросс-корреляция (коэффициент Пирсона по перекрывающемуся
    // участку) по целым сдвигам lag в [-MAX_LAG_SAMPLES, +MAX_LAG_SAMPLES].
    // R_corr(lag) = sum L[i]*R[i+lag] максимален, когда R[t] ~= L[t-lag]
    // (R — задержанная копия L на lag сэмплов) — то есть положительный lag
    // значит верхний (R) канал отстаёт от нижнего (L), звук пришёл в нижний
    // раньше — источник ниже линии микрофонов (BELOW).
    float bestCorr = -1.0f;
    int   bestLag  = 0;

    for (int lag = -MAX_LAG_SAMPLES; lag <= MAX_LAG_SAMPLES; lag++) {
        size_t iStart = (lag < 0) ? (size_t)(-lag) : 0;
        size_t iEnd   = (lag > 0) ? (DIRECTION_WINDOW_SAMPLES - (size_t)lag) : DIRECTION_WINDOW_SAMPLES;
        if (iEnd <= iStart) continue;

        float sumLR = 0, sumLL = 0, sumRR = 0;
        for (size_t i = iStart; i < iEnd; i++) {
            float l = s_windowL[i];
            float r = s_windowR[i + (size_t)lag];
            sumLR += l * r;
            sumLL += l * l;
            sumRR += r * r;
        }
        float denom = sqrtf(sumLL * sumRR);
        float corr = (denom > 1.0f) ? (sumLR / denom) : 0.0f;

        if (corr > bestCorr) {
            bestCorr = corr;
            bestLag  = lag;
        }
    }

    result.confidence = bestCorr > 0 ? bestCorr : 0;
    result.delayMs = (float)bestLag * 1000.0f / (float)SAMPLE_RATE_HZ;

    // "Мёртвая зона" вокруг нуля — доля от теоретического максимума задержки
    // при заданном MIC_SPACING_M. Внутри неё считаем "по центру": либо
    // источник действительно примерно на той же высоте, что и линия
    // микрофонов (в т.ч. любой азимут — метод его не различает, см.
    // direction.h), либо это шум оценки на тихом/смазанном событии.
    float maxTheoreticalDelayMs = MIC_SPACING_M / SPEED_OF_SOUND_MPS * 1000.0f;
    float deadbandMs = maxTheoreticalDelayMs * DIRECTION_CENTER_DEADBAND_FRAC;

    if (result.delayMs > deadbandMs) {
        result.side = Side::BELOW;
    } else if (result.delayMs < -deadbandMs) {
        result.side = Side::ABOVE;
    } else {
        result.side = Side::CENTER;
    }

    return result;
}

} // namespace Direction
