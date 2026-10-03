#include "audio_capture.h"
#include "config.h"

#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>

namespace AudioCapture {

static const i2s_port_t I2S_PORT = I2S_NUM_0;

// Сколько СЫРЫХ 32-битных I2S-слов читаем за один заход задачи. В стерео-
// режиме на одну пару (L,R) приходится 2 слова.
static const int I2S_READ_CHUNK_PAIRS = 128;
static const int I2S_READ_CHUNK_WORDS = I2S_READ_CHUNK_PAIRS * 2;

struct StereoSample { int16_t l; int16_t r; };

// Ёмкость потокового буфера между задачей захвата и потребителем — в парах,
// с запасом на несколько кадров, чтобы кратковременная задержка потребителя
// (например, вычисление направления в event_detector) не роняла сэмплы.
static const size_t STREAM_BUF_PAIRS = SAMPLE_RATE_HZ / 2; // 0.5 сек запаса
static const size_t STREAM_BUF_BYTES = STREAM_BUF_PAIRS * sizeof(StereoSample);

static StreamBufferHandle_t s_streamBuf = nullptr;
static TaskHandle_t s_captureTaskHandle = nullptr;
static volatile uint32_t s_droppedFrames = 0;

static void captureTask(void *arg) {
    // Буфер сырых 32-битных фреймов от I2S (INMP441 отдаёт 24 значащих бита,
    // выровненные по старшим битам 32-битного слова). В стерео-режиме слова
    // идут парами (левый слот, правый слот) — так работает I2S-периферия при
    // channel_format = RIGHT_LEFT.
    static int32_t rawBuf[I2S_READ_CHUNK_WORDS];
    static StereoSample pcmBuf[I2S_READ_CHUNK_PAIRS];

    // Порядок L/R слов внутри пары, который реально отдаёт I2S-периферия,
    // без реального железа под рукой гарантировать нельзя (зависит от
    // трактовки "первого" слова кадра конкретной ревизией драйвера/платы).
    // Если после монтажа "снизу/сверху" на экране окажутся перепутаны — не
    // нужно перепаивать, достаточно поставить MIC_SWAP_CHANNELS 1 в config.h.
    const int li = MIC_SWAP_CHANNELS ? 1 : 0;
    const int ri = MIC_SWAP_CHANNELS ? 0 : 1;

    for (;;) {
        size_t bytesRead = 0;
        esp_err_t err = i2s_read(I2S_PORT, rawBuf, sizeof(rawBuf), &bytesRead, portMAX_DELAY);
        if (err != ESP_OK || bytesRead == 0) {
            continue;
        }
        int wordsRead = bytesRead / sizeof(int32_t);
        int pairs = wordsRead / 2; // нечётного "хвоста" слова быть не должно, но на всякий случай отбрасываем

        for (int i = 0; i < pairs; i++) {
            // Верхние 16 бит 32-битного I2S-слова — как и в моно-версии.
            pcmBuf[i].l = (int16_t)(rawBuf[i * 2 + li] >> 16);
            pcmBuf[i].r = (int16_t)(rawBuf[i * 2 + ri] >> 16);
        }

        size_t bytesToSend = (size_t)pairs * sizeof(StereoSample);
        size_t sent = xStreamBufferSend(s_streamBuf, pcmBuf, bytesToSend, 0);
        if (sent < bytesToSend) {
            // Потребитель не успевает — часть пар потеряна. Считаем и едем
            // дальше: единичные потери некритичны, но если счётчик растёт
            // быстро — сигнал, что детектору не хватает производительности.
            s_droppedFrames++;
        }
    }
}

bool begin() {
    i2s_config_t i2sConfig = {};
    i2sConfig.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    i2sConfig.sample_rate = SAMPLE_RATE_HZ;
    i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT; // стерео: 2 микрофона на одной шине
    i2sConfig.communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S);
    i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2sConfig.dma_buf_count = 8;
    i2sConfig.dma_buf_len = 256;
    i2sConfig.use_apll = false;
    i2sConfig.tx_desc_auto_clear = false;
    i2sConfig.fixed_mclk = 0;

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_driver_install failed: %d\n", err);
        return false;
    }

    i2s_pin_config_t pinConfig = {};
    pinConfig.bck_io_num = PIN_I2S_BCLK;
    pinConfig.ws_io_num = PIN_I2S_WS;
    pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
    pinConfig.data_in_num = PIN_I2S_DIN;

    err = i2s_set_pin(I2S_PORT, &pinConfig);
    if (err != ESP_OK) {
        Serial.printf("[audio] i2s_set_pin failed: %d\n", err);
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT);

    s_streamBuf = xStreamBufferCreate(STREAM_BUF_BYTES, sizeof(StereoSample));
    if (!s_streamBuf) {
        Serial.println("[audio] failed to create stream buffer");
        return false;
    }

    // Задача захвата — на ядре 0, приоритет выше обычного (звук нельзя
    // прерывать надолго), но не максимальный, чтобы не мешать Wi-Fi-стеку.
    BaseType_t ok = xTaskCreatePinnedToCore(
        captureTask, "audio_capture", 4096, nullptr,
        configMAX_PRIORITIES - 2, &s_captureTaskHandle, 0);

    if (ok != pdPASS) {
        Serial.println("[audio] failed to create capture task");
        return false;
    }

    Serial.println("[audio] I2S capture started (stereo, 2 mics)");
    return true;
}

size_t readStereoSamples(int16_t *dstLeft, int16_t *dstRight, size_t maxPairs, uint32_t timeoutMs) {
    if (!s_streamBuf) return 0;

    // Scratch — статический, т.к. вызывается только из одной задачи
    // (event_detector::detectorTask), и maxPairs у нас всегда <= FRAME_SAMPLES.
    static StereoSample scratch[FRAME_SAMPLES];
    if (maxPairs > FRAME_SAMPLES) maxPairs = FRAME_SAMPLES;

    size_t maxBytes = maxPairs * sizeof(StereoSample);
    size_t got = xStreamBufferReceive(s_streamBuf, scratch, maxBytes, pdMS_TO_TICKS(timeoutMs));
    size_t pairs = got / sizeof(StereoSample);

    for (size_t i = 0; i < pairs; i++) {
        dstLeft[i]  = scratch[i].l;
        dstRight[i] = scratch[i].r;
    }
    return pairs;
}

size_t availableSamples() {
    if (!s_streamBuf) return 0;
    return xStreamBufferBytesAvailable(s_streamBuf) / sizeof(StereoSample);
}

uint32_t droppedFrames() {
    return s_droppedFrames;
}

} // namespace AudioCapture
