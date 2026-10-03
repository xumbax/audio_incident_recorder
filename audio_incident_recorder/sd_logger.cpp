#include "sd_logger.h"
#include "config.h"
#include "direction.h"

#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

namespace SdLogger {

namespace {
bool s_available = false;

// WAV-заголовок для моно 16-бит PCM.
struct WavHeader {
    char     riff[4]        = {'R','I','F','F'};
    uint32_t chunkSize      = 0; // заполняется после записи данных
    char     wave[4]        = {'W','A','V','E'};
    char     fmt[4]         = {'f','m','t',' '};
    uint32_t fmtSize        = 16;
    uint16_t audioFormat    = 1; // PCM
    uint16_t numChannels    = 1;
    uint32_t sampleRate     = SAMPLE_RATE_HZ;
    uint32_t byteRate       = SAMPLE_RATE_HZ * 2;
    uint16_t blockAlign     = 2;
    uint16_t bitsPerSample  = 16;
    char     data[4]        = {'d','a','t','a'};
    uint32_t dataSize       = 0; // заполняется после записи данных
};

void csvEscape(const char *in, char *out, size_t outSize) {
    // Простая защита от запятых/переводов строк в свободном тексте (описание
    // паттерна сирены и т.п.) — оборачиваем в кавычки, экранируем кавычки.
    size_t o = 0;
    if (o < outSize - 1) out[o++] = '"';
    for (const char *p = in; *p && o < outSize - 2; p++) {
        if (*p == '"') { if (o < outSize - 2) out[o++] = '"'; }
        if (*p == '\n' || *p == '\r') continue;
        out[o++] = *p;
    }
    if (o < outSize - 1) out[o++] = '"';
    out[o] = '\0';
}

} // namespace

bool begin() {
    // SD делит SPI-шину с TFT (см. config.h) — SPI уже поднят в display_ui,
    // здесь просто открываем SD на своём CS.
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("[sd] SD.begin() failed — карта не смонтирована");
        s_available = false;
        return false;
    }

    if (!SD.exists(SD_EVENTS_DIR)) {
        SD.mkdir(SD_EVENTS_DIR);
    }

    if (!SD.exists(SD_LOG_CSV_PATH)) {
        File f = SD.open(SD_LOG_CSV_PATH, FILE_WRITE);
        if (f) {
            f.println("timestamp,kind,label,duration_sec,peak_amp,peak_dbfs,avg_dbfs,direction,direction_delay_ms,direction_confidence,pattern_or_confidence,wav_path");
            f.close();
        }
    }

    s_available = true;
    Serial.println("[sd] SD OK");
    return true;
}

bool isAvailable() { return s_available; }

bool saveWav(const int16_t *samples, size_t numSamples, time_t startEpoch,
             char *outPath, size_t outPathSize) {
    if (!s_available || numSamples == 0) return false;

    struct tm tmInfo;
    localtime_r(&startEpoch, &tmInfo);
    char fname[48];
    snprintf(fname, sizeof(fname), "%04d%02d%02d_%02d%02d%02d.wav",
              tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday,
              tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);

    snprintf(outPath, outPathSize, "%s/%s", SD_EVENTS_DIR, fname);

    File f = SD.open(outPath, FILE_WRITE);
    if (!f) {
        Serial.printf("[sd] не удалось создать %s\n", outPath);
        return false;
    }

    WavHeader hdr;
    uint32_t dataBytes = (uint32_t)numSamples * sizeof(int16_t);
    hdr.dataSize = dataBytes;
    hdr.chunkSize = 36 + dataBytes;

    f.write((const uint8_t *)&hdr, sizeof(WavHeader));
    f.write((const uint8_t *)samples, dataBytes);
    f.close();

    return true;
}

bool appendEventCsv(const EventRecord &ev) {
    if (!s_available) return false;

    File f = SD.open(SD_LOG_CSV_PATH, FILE_APPEND);
    if (!f) return false;

    struct tm tmInfo;
    localtime_r(&ev.startEpoch, &tmInfo);
    char ts[32];
    // GCC не может статически доказать, что поля struct tm из localtime_r
    // ограничены обычным календарным диапазоном (формально tm_year — просто
    // int), поэтому -Wformat-truncation считает вход неограниченным и ругается
    // на теоретический наихудший случай. На практике here-and-now это всегда
    // нормальная календарная дата, буфер с запасом (32 байта на "ГГГГ-ММ-ДД
    // ЧЧ:ММ:СС") более чем достаточен.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
              tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday,
              tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
#pragma GCC diagnostic pop

    if (ev.kind == EventKind::SIREN) {
        char patternEsc[128];
        csvEscape(ev.siren.patternText, patternEsc, sizeof(patternEsc));
        f.printf("%s,SIREN,%s,%.1f,%d,%.1f,%.1f,%s,%.2f,%.2f,%s,%s\n",
                  ts, "siren", ev.durationSec, ev.peakAmp, ev.peakDbfs, ev.avgDbfs,
                  Direction::sideLabel(ev.directionSide), ev.directionDelayMs, ev.directionConfidence,
                  patternEsc, ev.wavPath);
    } else {
        const char *label = bangCategoryLabel(ev.bang.category);
        f.printf("%s,%s,%s,%.1f,%d,%.1f,%.1f,%s,%.2f,%.2f,confidence=%.2f,%s\n",
                  ts, eventKindLabel(ev.kind), label, ev.durationSec, ev.peakAmp,
                  ev.peakDbfs, ev.avgDbfs, Direction::sideLabel(ev.directionSide),
                  ev.directionDelayMs, ev.directionConfidence, ev.bang.confidence, ev.wavPath);
    }

    f.close();
    return true;
}

bool appendDiagLine(const char *line) {
    if (!s_available) return false;
    File f = SD.open(SD_DIAG_LOG_PATH, FILE_APPEND);
    if (!f) return false;
    f.println(line);
    f.close();
    return true;
}

} // namespace SdLogger
