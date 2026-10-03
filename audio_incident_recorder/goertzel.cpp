#include "goertzel.h"
#include <math.h>

void goertzelPrepare(GoertzelBin &bin, float freqHz, float sampleRate, int n) {
    bin.freqHz = freqHz;
    float k = 0.5f + ((float)n * freqHz) / sampleRate;
    float omega = (2.0f * (float)M_PI * k) / (float)n;
    bin.coeff = 2.0f * cosf(omega);
}

float goertzelPower(const int16_t *samples, int n, const GoertzelBin &bin) {
    float sPrev = 0.0f, sPrev2 = 0.0f;
    const float coeff = bin.coeff;
    for (int i = 0; i < n; i++) {
        float s = (float)samples[i] + coeff * sPrev - sPrev2;
        sPrev2 = sPrev;
        sPrev = s;
    }
    // |X(k)|^2, без нормировки на N — нам важны только относительные величины
    // между бинами/кадрами, абсолютная калибровка не нужна.
    return sPrev2 * sPrev2 + sPrev * sPrev - coeff * sPrev * sPrev2;
}

float frameRms(const int16_t *samples, int n) {
    if (n <= 0) return 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < n; i++) {
        float s = (float)samples[i];
        sumSq += (double)(s * s);
    }
    return sqrtf((float)(sumSq / n));
}

int16_t framePeakAbs(const int16_t *samples, int n) {
    int16_t peak = 0;
    for (int i = 0; i < n; i++) {
        int16_t a = samples[i] < 0 ? (int16_t)(-samples[i]) : samples[i];
        if (a > peak) peak = a;
    }
    return peak;
}

float frameZcr(const int16_t *samples, int n) {
    if (n < 2) return 0.0f;
    int crossings = 0;
    for (int i = 1; i < n; i++) {
        bool prevPos = samples[i - 1] >= 0;
        bool curPos  = samples[i] >= 0;
        if (prevPos != curPos) crossings++;
    }
    return (float)crossings / (float)(n - 1);
}
