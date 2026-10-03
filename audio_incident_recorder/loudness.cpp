#include "loudness.h"
#include <math.h>

namespace Loudness {

namespace {
const float FULL_SCALE = 32768.0f; // 2^15, полная шкала int16 сэмплов
const float FLOOR_AMPLITUDE = 1.0f; // ~-90 дБFS — не даём log() уйти в -inf в тишине
}

float toDbfs(float amplitude) {
    if (amplitude < FLOOR_AMPLITUDE) amplitude = FLOOR_AMPLITUDE;
    return 20.0f * log10f(amplitude / FULL_SCALE);
}

} // namespace Loudness
