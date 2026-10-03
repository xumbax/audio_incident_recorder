#include "display_ui.h"
#include "config.h"

#include <TFT_eSPI.h>
#include "cyr_font.h"      // подключать после TFT_eSPI.h — см. комментарий в cyr_font.h
#include "cyr_convert.h"
#include "direction.h"
#include <SPI.h>           // для SPI.begin() в безопасном режиме, см. begin() ниже
#include <time.h>
#include <string.h>
#include <stdio.h>

namespace DisplayUI {

namespace {

TFT_eSPI tft = TFT_eSPI();

// Встроенные шрифты TFT_eSPI — только ASCII, кириллицы в них нет. Экран
// рисуется кастомным шрифтом cyr_font.h (см. tools/gen_cyr_font.py) с
// поддержкой кириллицы; все строки перед выводом проходят через
// utf8ToCp1251() (cyr_convert.h) — компилятор кладёт русские литералы как
// UTF-8, а этот шрифт — однобайтовый, в раскладке CP1251.
int LINE_HEIGHT = 26;   // пересчитывается в begin() из реальной метрики шрифта
const int HEADER_Y      = 4;
int STATUS_Y             = 0;
int LOG_TOP_Y            = 0;
const int SCREEN_W      = 240;
const int SCREEN_H      = 320;
int MAX_LOG_LINES        = 0;

char s_statusLine[64] = "инициализация...";
char s_logLines[32][72]; // кольцевой буфер строк лога (с запасом сверх MAX_LOG_LINES)
int  s_logCount = 0;     // сколько строк реально записано (может быть > видимого)

uint32_t s_lastHeaderMs = 0;

// true по умолчанию — обычный режим работы. .ino может сбросить в false
// ДО begin() (см. display_ui.h) в безопасном режиме после серии падений
// подряд, чтобы не рисковать повторно на этом же коде, пока не разобрались
// с причиной (см. watchdog_diag.h).
bool s_enabled = true;

// Конвертирует UTF-8 -> шрифтовую кодировку в статический буфер и рисует.
void drawUtf8(const char *utf8, int x, int y, uint16_t color, uint16_t bg) {
    char buf[80];
    utf8ToCp1251(utf8, buf, sizeof(buf));
    tft.setTextColor(color, bg);
    tft.drawString(buf, x, y);
}

void drawHeader() {
    time_t now = time(nullptr);
    struct tm tmInfo;
    localtime_r(&now, &tmInfo);

    char buf[32];
    if (tmInfo.tm_year > 100) { // время получено (год после 2000)
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
    } else {
        snprintf(buf, sizeof(buf), "-- net vremeni --"); // время ещё не пришло по NTP — латиница, не критично
    }

    tft.fillRect(0, HEADER_Y, SCREEN_W, LINE_HEIGHT, TFT_BLACK);
    drawUtf8(buf, 4, HEADER_Y, TFT_CYAN, TFT_BLACK);
}

void redrawStatus() {
    tft.fillRect(0, STATUS_Y, SCREEN_W, LINE_HEIGHT, TFT_BLACK);
    drawUtf8(s_statusLine, 4, STATUS_Y, TFT_YELLOW, TFT_BLACK);
}

void redrawLog() {
    tft.fillRect(0, LOG_TOP_Y, SCREEN_W, SCREEN_H - LOG_TOP_Y, TFT_BLACK);

    int linesToShow = s_logCount < MAX_LOG_LINES ? s_logCount : MAX_LOG_LINES;
    for (int i = 0; i < linesToShow; i++) {
        // Самая свежая строка — сверху лога.
        int idx = (s_logCount - 1 - i) % 32;
        if (idx < 0) idx += 32;
        drawUtf8(s_logLines[idx], 4, LOG_TOP_Y + i * LINE_HEIGHT, TFT_WHITE, TFT_BLACK);
    }
}

} // namespace

void setEnabled(bool enabled) {
    s_enabled = enabled;
}

void begin() {
    if (!s_enabled) {
        // tft.init() ниже НЕ вызывается — а значит и общую SPI-шину (см.
        // config.h — TFT и SD сидят на одной шине) поднять некому. Делаем
        // это сами теми же пинами, которыми это сделал бы tft.init()
        // (см. ниже) — SdLogger::begin() рассчитывает, что к его вызову
        // шина уже поднята.
        SPI.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, -1);
        Serial.println("[display] отключён программно (безопасный режим) — работаем без экрана, статус/события дублируются в Serial");
        return;
    }

    // ВАЖНО: SPI.begin() здесь и в .ino НЕ вызываем — tft.init() поднимает
    // общую SPI-шину САМ внутри себя. На реальном железе (см. README,
    // "Устойчивость к сбоям") было подтверждено: если до tft.init() уже
    // вызвать SPI.begin() самостоятельно (как было раньше — для "общей
    // шины с SD"), внутренний повторный spi.begin(...) с ДРУГИМ SS-пином
    // (TFT_eSPI на ESP32-S3 без явного USE_HSPI_PORT/USE_FSPI_PORT
    // использует ссылку на тот же глобальный объект SPI, см.
    // TFT_eSPI/Processors/TFT_eSPI_ESP32_S3.c) стабильно роняет устройство
    // в Guru Meditation Error (StoreProhibited) сразу после старта — это и
    // было причиной "падения на голой плате". SD, которая физически сидит
    // на той же шине, спокойно работает и без нашего отдельного
    // SPI.begin() — ей достаточно того, что tft.init() уже поднял шину.
    tft.init();
    tft.setRotation(0); // портрет, 240x320 — как в weather_display
    tft.setFreeFont(&CyrFont20);
    tft.setTextDatum(TL_DATUM); // (x,y) = верхний левый угол текста, не зависит от базовой линии шрифта
    tft.fillScreen(TFT_BLACK);

    LINE_HEIGHT = tft.fontHeight() + 2;
    STATUS_Y = HEADER_Y + LINE_HEIGHT + 4;
    LOG_TOP_Y = STATUS_Y + LINE_HEIGHT + 6;
    MAX_LOG_LINES = (SCREEN_H - LOG_TOP_Y) / LINE_HEIGHT;
    if (MAX_LOG_LINES < 1) MAX_LOG_LINES = 1;
    if (MAX_LOG_LINES > 32) MAX_LOG_LINES = 32;

    pinMode(PIN_TFT_BL, OUTPUT);
    digitalWrite(PIN_TFT_BL, HIGH);

    // Диагностика "экран вообще отвечает по SPI?" — читаем байт ID через
    // MISO (безопасная транзакция, ничего не пишет в память, только шлёт/
    // читает по шине). Если MISO не разведён/экран не подключён, получим
    // "залипший" 0x00 или 0xFF — не фатально (рисовать можно и вслепую), но
    // печатаем предупреждение, чтобы не гадать по темному экрану, в чём
    // дело.
    uint8_t id = tft.readcommand8(0x04, 1); // RDDID, manufacturer ID byte
    if (id == 0x00 || id == 0xFF) {
        Serial.printf("[display] предупреждение: экран не отвечает по SPI (ID=0x%02X) — проверьте, подключён ли ILI9341 и разведён ли MISO (пин %d)\n", id, PIN_TFT_MISO);
    } else {
        Serial.printf("[display] экран отвечает по SPI (ID=0x%02X)\n", id);
    }

    drawHeader();
    redrawStatus();
    redrawLog();
}

void tick() {
    if (!s_enabled) return;

    uint32_t now = millis();
    if (now - s_lastHeaderMs >= 1000) {
        drawHeader();
        s_lastHeaderMs = now;
    }
}

void setStatusLine(const char *text) {
    strncpy(s_statusLine, text, sizeof(s_statusLine) - 1);
    s_statusLine[sizeof(s_statusLine) - 1] = '\0';

    if (!s_enabled) {
        Serial.printf("[display] статус: %s\n", s_statusLine);
        return;
    }
    redrawStatus();
}

void pushFinishedEvent(const EventRecord &ev) {
    struct tm tmInfo;
    localtime_r(&ev.startEpoch, &tmInfo);

    // Экран узкий (240px) — длинное patternText всё равно не влезет в
    // строку, поэтому сознательно обрезаем его до 24 символов через
    // precision у %s (заодно снимает предупреждение компилятора о
    // возможном переполнении: без precision снаружи не видно, что вход
    // ограничен, а массив patternText — 96 байт).
    // Громкость показываем пиковую (см. loudness.h — это дБFS, не
    // калиброванный SPL): для короткого взгляда на экран "насколько громко
    // было в самый пиковый момент" информативнее, чем среднее по событию;
    // среднее (avgDbfs) пишем в CSV на SD для более полного анализа.
    // Угол места — однобуквенной меткой (Н/В/Ц/?, см. direction.h) в конце
    // строки, экран узкий, места на полное слово нет.
    char line[72];
    if (ev.kind == EventKind::SIREN) {
        // Precision у последнего %s (короткая метка направления) — не для
        // экономии места (она и так короткая), а чтобы GCC мог статически
        // доказать границу вывода в line[]: без precision тип "const char*"
        // из функции для -Wformat-truncation выглядит неограниченным.
        snprintf(line, sizeof(line), "%02d:%02d СИРЕНА %.20s %.0fдБ %.4s",
                  tmInfo.tm_hour, tmInfo.tm_min, ev.siren.patternText, ev.peakDbfs,
                  Direction::sideShortLabel(ev.directionSide));
    } else {
        snprintf(line, sizeof(line), "%02d:%02d %.16s %.0fс %.0fдБ %.4s",
                  tmInfo.tm_hour, tmInfo.tm_min, bangCategoryLabel(ev.bang.category),
                  ev.durationSec, ev.peakDbfs, Direction::sideShortLabel(ev.directionSide));
    }

    int idx = s_logCount % 32;
    strncpy(s_logLines[idx], line, sizeof(s_logLines[idx]) - 1);
    s_logLines[idx][sizeof(s_logLines[idx]) - 1] = '\0';
    s_logCount++;

    if (s_enabled) {
        redrawLog();
    } else {
        Serial.printf("[display] событие в лог (экран отключён): %s\n", line);
    }
    setStatusLine("прослушивание...");
}

} // namespace DisplayUI
