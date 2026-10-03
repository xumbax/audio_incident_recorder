#include "event_detector.h"
#include "config.h"
#include "goertzel.h"
#include "audio_capture.h"
#include "siren_analyzer.h"
#include "bang_classifier.h"
#include "sd_logger.h"
#include "loudness.h"
#include "direction.h"

#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string.h>
#include <time.h>
#include <math.h>

namespace EventDetector {

namespace {

enum class DetState { IDLE, CAPTURING, SIREN_TRACKING };

// --- Буферы в PSRAM ---
int16_t *s_preroll = nullptr;      // кольцевой пре-ролл буфер
size_t   s_prerollWriteIdx = 0;

int16_t *s_capture = nullptr;      // линейный буфер захвата текущего события
size_t   s_capLen = 0;
bool     s_capFull = false;

// --- Состояние детектора (трогает только detectorTask) ---
DetState s_state = DetState::IDLE;
float    s_noiseFloor = NOISE_FLOOR_MIN;
int      s_consecLoud = 0;
int      s_consecQuiet = 0;
int16_t  s_eventPeak = 0;
time_t   s_eventStartEpoch = 0;
uint64_t s_eventStartSampleCount = 0;
uint64_t s_totalSampleCount = 0;
float    s_toneStreakSec = 0;

// громкость: копим сумму квадратов RMS по кадрам события, чтобы в конце
// получить точный общий RMS за весь захват (среднее RMS^2 по равным по
// размеру кадрам математически равно общему RMS^2 по всем сэмплам сразу —
// пересчитывать по сырым сэмплам не нужно)
double s_rmsSquaredAccum = 0;
uint32_t s_activeFrameCount = 0;

// направление (угол места, снизу/сверху) — снимок, сделанный один раз в
// момент старта события (см. startEvent()/Direction::estimate())
Direction::Estimate s_eventDirection;

// сегментация вкл/выкл внутри события (паттерн сирены)
const int SEG_START_FRAMES = 1;
const int SEG_END_FRAMES   = 5; // 100 мс тишины внутри события закрывает отрезок
bool  s_segOn = false;
int   s_segConsecLoud = 0;
int   s_segConsecQuiet = 0;
float s_segCurrentOnSec = 0;
float s_gapAccumSec = 0;
SirenResult s_workingSiren;

const int SIREN_SEQ_GAP_FRAMES =
    (int)((SIREN_SEQUENCE_GAP_SEC * 1000.0f) / FRAME_MS + 0.5f);

// --- Обмен с внешним миром (main loop / экран) ---
SemaphoreHandle_t s_mutex = nullptr;
LiveStatus  s_liveStatus;
bool        s_hasFinished = false;
EventRecord s_finishedEvent;

// ---------------------------------------------------------------------
// Пре-ролл кольцевой буфер
// ---------------------------------------------------------------------
void prerollPush(const int16_t *frame, int n) {
    for (int i = 0; i < n; i++) {
        s_preroll[s_prerollWriteIdx] = frame[i];
        s_prerollWriteIdx++;
        if (s_prerollWriteIdx >= PREROLL_SAMPLES) s_prerollWriteIdx = 0;
    }
}

void prerollCopyInto(int16_t *dst, size_t count) {
    if (count > PREROLL_SAMPLES) count = PREROLL_SAMPLES;
    size_t start = (s_prerollWriteIdx + PREROLL_SAMPLES - count) % PREROLL_SAMPLES;
    for (size_t i = 0; i < count; i++) {
        dst[i] = s_preroll[(start + i) % PREROLL_SAMPLES];
    }
}

// ---------------------------------------------------------------------
// Публикация статуса/событий наружу (под мьютексом)
// ---------------------------------------------------------------------
void updateLiveStatus(float elapsedSec) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_liveStatus.inEvent = (s_state != DetState::IDLE);
        s_liveStatus.isSirenCandidate = (s_state == DetState::SIREN_TRACKING);
        s_liveStatus.elapsedSec = elapsedSec;
        s_liveStatus.peakDbfs = Loudness::toDbfs((float)s_eventPeak);
        s_liveStatus.directionSide = s_eventDirection.side;
        xSemaphoreGive(s_mutex);
    }
}

void publishFinishedEvent(const EventRecord &ev) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_finishedEvent = ev;
        s_hasFinished = true;
        xSemaphoreGive(s_mutex);
    }
}

// ---------------------------------------------------------------------
// Сегментация вкл/выкл (для паттерна сирены)
// ---------------------------------------------------------------------
void closeCurrentSegmentIfOpen() {
    if (!s_segOn) return;
    if (s_workingSiren.segmentCount < MAX_SIREN_SEGMENTS) {
        s_workingSiren.segmentSec[s_workingSiren.segmentCount] = s_segCurrentOnSec;
        s_workingSiren.segmentCount++;
    } else {
        s_workingSiren.truncated = true;
    }
    s_segOn = false;
}

void updateSegmentation(bool loud, bool quiet) {
    if (!s_segOn) {
        if (s_workingSiren.segmentCount > 0) {
            s_gapAccumSec += FRAME_MS / 1000.0f;
        }
        if (loud) s_segConsecLoud++; else s_segConsecLoud = 0;

        if (s_segConsecLoud >= SEG_START_FRAMES) {
            if (s_workingSiren.segmentCount > 0) {
                int gi = s_workingSiren.segmentCount - 1;
                if (gi < MAX_SIREN_SEGMENTS) s_workingSiren.gapSec[gi] = s_gapAccumSec;
            }
            s_gapAccumSec = 0;
            s_segOn = true;
            s_segCurrentOnSec = 0;
            s_segConsecQuiet = 0;
        }
    } else {
        s_segCurrentOnSec += FRAME_MS / 1000.0f;
        if (quiet) s_segConsecQuiet++; else s_segConsecQuiet = 0;

        if (s_segConsecQuiet >= SEG_END_FRAMES) {
            closeCurrentSegmentIfOpen();
            s_segConsecLoud = 0;
            s_gapAccumSec = 0;
        }
    }
}

// ---------------------------------------------------------------------
// Старт / финал события
// ---------------------------------------------------------------------
void startEvent() {
    s_state = DetState::CAPTURING;
    s_eventPeak = 0;
    s_eventStartEpoch = time(nullptr);
    s_eventStartSampleCount = s_totalSampleCount;
    s_toneStreakSec = 0;
    s_consecQuiet = 0;
    s_rmsSquaredAccum = 0;
    s_activeFrameCount = 0;
    // Направление считаем один раз, сразу здесь — фронт атаки события ещё
    // свежий в скользящем окне Direction (см. direction.h/.cpp).
    s_eventDirection = Direction::estimate();

    prerollCopyInto(s_capture, PREROLL_SAMPLES);
    s_capLen = PREROLL_SAMPLES;
    s_capFull = false;

    s_workingSiren = SirenResult();
    s_segOn = false;
    s_segConsecLoud = 0;
    s_segConsecQuiet = 0;
    s_segCurrentOnSec = 0;
    s_gapAccumSec = 0;
}

void finishEvent() {
    closeCurrentSegmentIfOpen();

    EventRecord ev;
    ev.startEpoch = s_eventStartEpoch;
    ev.durationSec = (float)(s_totalSampleCount - s_eventStartSampleCount) / (float)SAMPLE_RATE_HZ;
    ev.peakAmp = s_eventPeak;
    ev.peakDbfs = Loudness::toDbfs((float)s_eventPeak);
    {
        float meanSquare = (float)(s_rmsSquaredAccum / (s_activeFrameCount > 0 ? s_activeFrameCount : 1));
        ev.avgDbfs = Loudness::toDbfs(sqrtf(meanSquare));
    }
    ev.directionSide = s_eventDirection.side;
    ev.directionDelayMs = s_eventDirection.delayMs;
    ev.directionConfidence = s_eventDirection.confidence;

    if (s_state == DetState::SIREN_TRACKING) {
        ev.kind = EventKind::SIREN;
        SirenAnalyzer::finalize(s_workingSiren);
        ev.siren = s_workingSiren;
    } else {
        ev.bang = classifyBang(s_capture, s_capLen);
        // Событие громкое и продолжительное, но так и не признанное тональным —
        // скорее всего не одиночный хлопок, а какой-то длящийся шум (стройка,
        // сигнализация другого типа и т.п.). Порог — три "окна подтверждения тона".
        ev.kind = (ev.durationSec > TONE_CONFIRM_SEC * 3.0f) ? EventKind::LONG_NOISE : EventKind::BANG;
    }

    if (s_capLen > 0) {
        SdLogger::saveWav(s_capture, s_capLen, ev.startEpoch, ev.wavPath, sizeof(ev.wavPath));
    }
    SdLogger::appendEventCsv(ev);

    publishFinishedEvent(ev);

    s_state = DetState::IDLE;
    s_consecLoud = 0;
    s_consecQuiet = 0;
    updateLiveStatus(0);
}

// ---------------------------------------------------------------------
// Обработка одного кадра (FRAME_SAMPLES сэмплов)
// ---------------------------------------------------------------------
void processFrame(const int16_t *frame) {
    s_totalSampleCount += FRAME_SAMPLES;

    float   rms  = frameRms(frame, FRAME_SAMPLES);
    int16_t peak = framePeakAbs(frame, FRAME_SAMPLES);

    prerollPush(frame, FRAME_SAMPLES);

    bool loud  = rms > s_noiseFloor * THRESH_START_RATIO;
    bool quiet = rms < s_noiseFloor * THRESH_END_RATIO;

    if (s_state == DetState::IDLE) {
        if (!loud) {
            s_noiseFloor = s_noiseFloor * (1.0f - NOISE_FLOOR_EMA_ALPHA) + rms * NOISE_FLOOR_EMA_ALPHA;
            if (s_noiseFloor < NOISE_FLOOR_MIN) s_noiseFloor = NOISE_FLOOR_MIN;
        }

        s_consecLoud = loud ? s_consecLoud + 1 : 0;
        if (s_consecLoud >= START_FRAMES_NEEDED) {
            startEvent();
        }
        return;
    }

    // --- внутри события ---
    if (peak > s_eventPeak) s_eventPeak = peak;
    s_rmsSquaredAccum += (double)rms * (double)rms;
    s_activeFrameCount++;

    if (s_state == DetState::CAPTURING && !s_capFull) {
        size_t room = CAPTURE_MAX_SAMPLES - s_capLen;
        size_t toCopy = room < (size_t)FRAME_SAMPLES ? room : (size_t)FRAME_SAMPLES;
        memcpy(s_capture + s_capLen, frame, toCopy * sizeof(int16_t));
        s_capLen += toCopy;
        if (s_capLen >= CAPTURE_MAX_SAMPLES) s_capFull = true;
    }

    updateSegmentation(loud, quiet);

    if (s_state == DetState::CAPTURING) {
        float ratio = SirenAnalyzer::toneRatio(frame, FRAME_SAMPLES);
        if (loud && ratio >= TONE_RATIO_THRESHOLD) {
            s_toneStreakSec += FRAME_MS / 1000.0f;
        } else if (loud) {
            s_toneStreakSec = 0; // громко, но не в тон — сбрасываем гипотезу "сирена"
        }
        // в тихих кадрах стрик не трогаем (короткая пауза внутри серии гудков
        // не должна откатывать распознавание уже идущей сирены)

        if (s_toneStreakSec >= TONE_CONFIRM_SEC) {
            s_state = DetState::SIREN_TRACKING;
        }
    }

    if (quiet) s_consecQuiet++; else s_consecQuiet = 0;

    bool shouldFinish = false;
    if (s_state == DetState::CAPTURING) {
        shouldFinish = s_consecQuiet >= END_FRAMES_NEEDED;
    } else {
        shouldFinish = s_consecQuiet >= SIREN_SEQ_GAP_FRAMES;
    }

    float elapsedSec = (float)(s_totalSampleCount - s_eventStartSampleCount) / (float)SAMPLE_RATE_HZ;
    if (elapsedSec >= MAX_EVENT_SEC) shouldFinish = true;

    if (shouldFinish) {
        finishEvent();
        return;
    }

    updateLiveStatus(elapsedSec);
}

// ---------------------------------------------------------------------
// Задача
// ---------------------------------------------------------------------
void detectorTask(void *arg) {
    esp_task_wdt_add(nullptr);

    static int16_t leftBuf[FRAME_SAMPLES];
    static int16_t rightBuf[FRAME_SAMPLES];
    static int16_t monoBuf[FRAME_SAMPLES];
    size_t filled = 0;

    for (;;) {
        size_t got = AudioCapture::readStereoSamples(leftBuf + filled, rightBuf + filled,
                                                       FRAME_SAMPLES - filled, 1000);
        filled += got;

        esp_task_wdt_reset();

        if (filled < (size_t)FRAME_SAMPLES) {
            continue;
        }

        // Моно-конвейер (вся существующая детекция/классификация — пороги
        // громкости, тон сирены, признаки хлопка) работает как и раньше, на
        // среднем L/R, теперь уже настоящем стерео вместо единственного
        // канала. Направление считается ОТДЕЛЬНО, по сырым не усреднённым
        // каналам (см. Direction::pushFrame) — усреднение стирает разницу
        // времени прихода, которая и нужна для оценки угла места (снизу/сверху).
        for (size_t i = 0; i < (size_t)FRAME_SAMPLES; i++) {
            monoBuf[i] = (int16_t)(((int32_t)leftBuf[i] + (int32_t)rightBuf[i]) / 2);
        }

        Direction::pushFrame(leftBuf, rightBuf, FRAME_SAMPLES);
        processFrame(monoBuf);
        filled = 0;
    }
}

} // namespace

bool begin() {
    s_preroll = (int16_t *)heap_caps_malloc(PREROLL_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_capture = (int16_t *)heap_caps_malloc(CAPTURE_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_preroll || !s_capture) {
        Serial.println("[detector] не удалось выделить буферы в PSRAM — проверьте, что PSRAM включена в настройках платы (Tools -> PSRAM: OPI PSRAM)");
        return false;
    }
    memset(s_preroll, 0, PREROLL_SAMPLES * sizeof(int16_t));

    if (!Direction::begin()) {
        // Не фатально: direction.cpp уже вывел причину в Serial, вся основная
        // детекция (сирены/хлопки/громкость) продолжает работать как раньше,
        // просто без оценки угла места (снизу/сверху).
        Serial.println("[detector] продолжаем без определения направления");
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        Serial.println("[detector] не удалось создать мьютекс");
        return false;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        detectorTask, "event_detector", 8192, nullptr,
        configMAX_PRIORITIES - 3, nullptr, 1);

    if (ok != pdPASS) {
        Serial.println("[detector] не удалось создать задачу детектора");
        return false;
    }

    Serial.println("[detector] запущен");
    return true;
}

LiveStatus getLiveStatus() {
    LiveStatus copy;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        copy = s_liveStatus;
        xSemaphoreGive(s_mutex);
    }
    return copy;
}

bool popFinishedEvent(EventRecord &out) {
    bool has = false;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_hasFinished) {
            out = s_finishedEvent;
            s_hasFinished = false;
            has = true;
        }
        xSemaphoreGive(s_mutex);
    }
    return has;
}

} // namespace EventDetector
