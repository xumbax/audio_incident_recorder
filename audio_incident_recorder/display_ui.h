// display_ui.h
// Экран ILI9341 240x320 (портретная ориентация, как в weather_display):
// шапка с временем/Wi-Fi, строка текущего статуса, хвост лога последних
// событий.
#pragma once
#include "event_types.h"

namespace DisplayUI {

// Вызвать ДО begin(), если экран нужно программно отключить (см. .ino —
// используется для "безопасного режима" после серии падений подряд, см.
// watchdog_diag.h). При enabled=false begin()/tick() ничего не делают, а
// setStatusLine()/pushFinishedEvent() дублируют то же самое в Serial, чтобы
// статус не терялся, даже если экрана физически нет или он не подключён.
void setEnabled(bool enabled);

void begin();

// Верхняя строка: часы + значок Wi-Fi. Дёшево, можно звать часто — сама
// внутри решает, действительно ли пора перерисовывать.
void tick();

// Текущий статус ("прослушивание...", или живой счётчик активного события).
void setStatusLine(const char *text);

// Добавить завершённое событие в хвост лога на экране (и обновить статус
// обратно на "прослушивание").
void pushFinishedEvent(const EventRecord &ev);

} // namespace DisplayUI
