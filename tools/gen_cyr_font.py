#!/usr/bin/env python3
"""
Генератор кастомного шрифта для TFT_eSPI (формат GFXfont, совместимый с
Adafruit_GFX free fonts) с поддержкой кириллицы.

Идея (стандартная для TFT_eSPI/Arduino, т.к. встроенные шрифты библиотеки —
только ASCII): GFXfont использует однобайтовые коды (uint8_t), поэтому
кириллицу пихаем в диапазон 0xC0-0xFF/0xA8/0xB8 — ТОЧНО как в кодовой
странице Windows-1251 (CP1251). Это стандартная, а не самопальная раскладка:
0xA8/0xB8 = Ё/ё, 0xC0-0xDF = А-Я, 0xE0-0xFF = а-я. В прошивке строка сначала
прогоняется через utf8_to_cp1251() (см. cyr_convert.h), а уже однобайтовый
результат печатается через шрифт.

Рендерит глифы через Pillow (FreeType) из DejaVu Sans Bold, пакует биты
1bpp MSB-first непрерывным потоком — так же, как оригинальный fontconvert.c
из Adafruit_GFX/TFT_eSPI, чтобы результат читался стандартным рендерером
TFT_eSPI без переделок.

Дополнительно пишет self-check PNG, отрисованный ТЕМ ЖЕ алгоритмом
распаковки бит, что и Adafruit_GFX::drawChar — чтобы визуально проверить
результат до заливки в устройство (у нас нет реального экрана под рукой).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
PIXEL_SIZE = 20  # размер шрифта в пикселях (em)
OUT_HEADER = "/root/audio_incident_recorder/audio_incident_recorder/cyr_font.h"
FONT_VAR = "CyrFont20"

# --- Раскладка байт -> символ (CP1251-совместимая) ---
def build_charmap():
    cmap = {}
    for b in range(0x20, 0x7F):  # печатаемый ASCII
        cmap[b] = chr(b)
    cmap[0xA8] = "Ё"  # Ё
    cmap[0xB8] = "ё"  # ё
    for i in range(32):
        cmap[0xC0 + i] = chr(0x0410 + i)  # А-Я
        cmap[0xE0 + i] = chr(0x0430 + i)  # а-я
    return cmap

CHARMAP = build_charmap()
FIRST = 0x20
LAST = 0xFF

def render_glyph(font, ch):
    canvas_size = PIXEL_SIZE * 3
    origin = PIXEL_SIZE
    canvas = Image.new("L", (canvas_size, canvas_size), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((origin, origin), ch, font=font, fill=255, anchor="ls")
    bbox = canvas.getbbox()
    advance = font.getlength(ch)
    if bbox is None:
        return {"w": 0, "h": 0, "xoff": 0, "yoff": 0, "adv": round(advance), "bits": []}
    left, top, right, bottom = bbox
    w, h = right - left, bottom - top
    xoff, yoff = left - origin, top - origin
    cropped = canvas.crop((left, top, right, bottom))
    bits = []
    for y in range(h):
        for x in range(w):
            bits.append(1 if cropped.getpixel((x, y)) >= 128 else 0)
    return {"w": w, "h": h, "xoff": xoff, "yoff": yoff, "adv": round(advance), "bits": bits}

def pack_bits(all_bits):
    out = bytearray()
    cur = 0
    n = 0
    for b in all_bits:
        cur = (cur << 1) | b
        n += 1
        if n == 8:
            out.append(cur)
            cur, n = 0, 0
    if n:
        out.append(cur << (8 - n))
    return bytes(out)

def main():
    font = ImageFont.truetype(FONT_PATH, PIXEL_SIZE)
    ascent, descent = font.getmetrics()

    glyphs = []          # metadata per code FIRST..LAST
    bitmap_stream = []   # плоский поток бит всех глифов подряд
    total = LAST - FIRST + 1
    for code in range(FIRST, LAST + 1):
        ch = CHARMAP.get(code)
        if ch is None:
            glyphs.append({"bo": len(bitmap_stream) // 8 if bitmap_stream else 0,
                            "w": 0, "h": 0, "adv": PIXEL_SIZE // 2, "xoff": 0, "yoff": 0})
            continue
        g = render_glyph(font, ch)
        bo_bits = len(bitmap_stream)
        bo_bytes = bo_bits // 8
        assert bo_bits % 8 == 0, "битовый поток должен быть кратен 8 на границе глифа"
        glyphs.append({"bo": bo_bytes, "w": g["w"], "h": g["h"],
                        "adv": g["adv"], "xoff": g["xoff"], "yoff": g["yoff"]})
        bitmap_stream.extend(g["bits"])
        # выравниваем поток по границе байта после каждого глифа —
        # чуть менее плотная упаковка, зато bitmapOffset всегда целое
        # число байт, что проще и надёжнее на этапе кодогенерации
        while len(bitmap_stream) % 8 != 0:
            bitmap_stream.append(0)

    bitmap_bytes = pack_bits(bitmap_stream)

    with open(OUT_HEADER, "w", encoding="utf-8") as f:
        f.write("// cyr_font.h — АВТОСГЕНЕРИРОВАНО tools/gen_cyr_font.py, не редактировать руками.\n")
        f.write(f"// Источник: {FONT_PATH}, размер {PIXEL_SIZE}px. Формат — GFXfont\n")
        f.write("// (Adafruit_GFX free font), однобайтовые коды 0x20-0xFF, кириллица в\n")
        f.write("// раскладке CP1251 (см. cyr_convert.h для UTF-8 -> CP1251).\n")
        f.write("#pragma once\n#include <Arduino.h>\n")
        f.write("// GFXglyph/GFXfont — типы из TFT_eSPI/Fonts/GFXFF/gfxfont.h. НЕ подключаем\n")
        f.write("// этот файл сами: TFT_eSPI.h уже подключает его (и объявляет эти типы)\n")
        f.write("// САМ, но ТОЛЬКО если в User_Setup.h стоит '#define LOAD_GFXFF' — без\n")
        f.write("// этого объявления типов не будет вообще (не ошибка \"файл не найден\", а\n")
        f.write("// непонятное \"GFXglyph does not name a type\" ниже по этому файлу).\n")
        f.write("// Раньше здесь был свой '#include <Fonts/GFXFF/gfxfont.h>' — убрали: он не\n")
        f.write("// спасал от забытого LOAD_GFXFF (сам этот файл тоже завёрнут в '#ifdef\n")
        f.write("// LOAD_GFXFF'), а только незаметно тянул в каждый .cpp, где подключён этот\n")
        f.write("// файл, ещё и все ~48 стандартных шрифтов Adafruit_GFX (лишний флеш).\n")
        f.write("// Поэтому — явная проверка с понятным сообщением вместо каскада невнятных\n")
        f.write("// ошибок компилятора:\n")
        f.write("#ifndef LOAD_GFXFF\n")
        f.write('#error "LOAD_GFXFF не определён. Добавьте \'#define LOAD_GFXFF\' в TFT_eSPI/User_Setup.h (см. README.md, раздел \'Настройка окружения\') — без него кириллический шрифт не соберётся, и подключать этот файл (cyr_font.h) нужно ПОСЛЕ <TFT_eSPI.h>."\n')
        f.write("#endif\n\n")

        f.write(f"const uint8_t {FONT_VAR}Bitmaps[] PROGMEM = {{\n")
        for i in range(0, len(bitmap_bytes), 16):
            chunk = bitmap_bytes[i:i+16]
            f.write("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",\n")
        f.write("};\n\n")

        f.write(f"const GFXglyph {FONT_VAR}Glyphs[] PROGMEM = {{\n")
        for g in glyphs:
            f.write(f"    {{ {g['bo']}, {g['w']}, {g['h']}, {g['adv']}, {g['xoff']}, {g['yoff']} }},\n")
        f.write("};\n\n")

        y_advance = ascent + descent + 2
        f.write(f"const GFXfont {FONT_VAR} PROGMEM = {{\n")
        f.write(f"    (uint8_t *){FONT_VAR}Bitmaps, (GFXglyph *){FONT_VAR}Glyphs,\n")
        f.write(f"    0x{FIRST:02X}, 0x{LAST:02X}, {y_advance}\n")
        f.write("};\n")

    print(f"OK: {OUT_HEADER}")
    print(f"глифов: {total}, байт битмапов: {len(bitmap_bytes)}, yAdvance: {y_advance}")

    # --- self-check: тем же алгоритмом, что Adafruit_GFX::drawChar,
    # рендерим тестовую строку в PNG для визуальной проверки ---
    test_lines = [
        "СИРЕНА 47с непрерывная",
        "ХЛОПОК похоже на выстрел",
        "3 отрезка(ов) по ~8с (пауза ~2с)",
        "12:34:56 прослушивание...",
        "похоже на удар по металлу",
    ]
    scale = 4
    img = Image.new("L", (240 * scale, 140 * scale), 0)
    px = img.load()

    def draw_char_selfcheck(code, cx, cy):
        idx = code - FIRST
        g = glyphs[idx]
        if g["w"] == 0 or g["h"] == 0:
            return
        bo = g["bo"]
        bit_idx = bo * 8
        for yy in range(g["h"]):
            for xx in range(g["w"]):
                byte = bitmap_bytes[bit_idx // 8]
                bit = 0x80 >> (bit_idx % 8)
                bit_idx += 1
                if byte & bit:
                    x = (cx + g["xoff"] + xx) * scale
                    y = (cy + g["yoff"] + yy) * scale
                    if 0 <= x < img.width - scale and 0 <= y < img.height - scale:
                        for dy in range(scale):
                            for dx in range(scale):
                                px[x + dx, y + dy] = 255

    inv_cmap = {v: k for k, v in CHARMAP.items()}
    cy = 20
    for line in test_lines:
        cx = 4
        for ch in line:
            code = inv_cmap.get(ch)
            if code is None:
                cx += PIXEL_SIZE // 2
                continue
            draw_char_selfcheck(code, cx, cy)
            cx += glyphs[code - FIRST]["adv"]
        cy += y_advance + 4

    png_path = "/root/audio_incident_recorder/tools/cyr_font_selfcheck.png"
    img.save(png_path)
    print(f"self-check PNG: {png_path}")

if __name__ == "__main__":
    main()
