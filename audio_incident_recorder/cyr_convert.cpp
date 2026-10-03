#include "cyr_convert.h"
#include <stdint.h>

size_t utf8ToCp1251(const char *utf8, char *out, size_t outSize) {
    if (outSize == 0) return 0;

    size_t o = 0;
    const unsigned char *p = (const unsigned char *)utf8;

    while (*p && o + 1 < outSize) {
        unsigned char c = *p;
        uint32_t cp;

        if (c < 0x80) {
            cp = c;
            p += 1;
        } else if ((c & 0xE0) == 0xC0 && p[1]) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else {
            // Неподдерживаемая (например, 4-байтовая) или битая
            // UTF-8-последовательность — пропускаем байт и едем дальше,
            // не роняем всю строку из-за одного "экзотического" символа.
            p += 1;
            continue;
        }

        char outCh;
        if (cp < 0x80) {
            outCh = (char)cp;
        } else if (cp == 0x0401) {
            outCh = (char)0xA8; // Ё
        } else if (cp == 0x0451) {
            outCh = (char)0xB8; // ё
        } else if (cp >= 0x0410 && cp <= 0x042F) {
            outCh = (char)(0xC0 + (cp - 0x0410)); // А-Я
        } else if (cp >= 0x0430 && cp <= 0x044F) {
            outCh = (char)(0xE0 + (cp - 0x0430)); // а-я
        } else {
            outCh = '?'; // символ вне поддерживаемого набора (см. cyr_font.h)
        }

        out[o++] = outCh;
    }

    out[o] = '\0';
    return o;
}
