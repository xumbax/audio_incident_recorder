#include "event_types.h"

const char *bangCategoryLabel(BangCategory c) {
    switch (c) {
        case BangCategory::THUNDER:   return "похоже на грозу";
        case BangCategory::GUNSHOT:   return "похоже на выстрел";
        case BangCategory::EXPLOSION: return "похоже на взрыв";
        case BangCategory::METAL:     return "похоже на удар по металлу";
        case BangCategory::UNKNOWN:
        default:                      return "не определено";
    }
}

const char *eventKindLabel(EventKind k) {
    switch (k) {
        case EventKind::SIREN:      return "СИРЕНА";
        case EventKind::BANG:       return "ХЛОПОК";
        case EventKind::LONG_NOISE: return "ШУМ";
        default:                    return "?";
    }
}
