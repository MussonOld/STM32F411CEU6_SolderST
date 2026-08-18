/**
 * @file display.c
 * @brief Модуль управления дисплеем. Оптимизирован для снижения цикломатической сложности.
 */

#include "display.h"
#include "st7789.h"
#include <string.h>

static const Display_Driver_t *pDisp = NULL;

/* Слоты для SmartPrint */
typedef struct {
    char text[48];
    bool active;
} TextSlot_t;

/* Было 64 — слоты инфозоны (90, 91: "Err EEPROM", диагностика hal=N)
   оказались за пределами массива и молча игнорировались и
   SmartPrint, и ClearSlot. Индикатор реальной аппаратной
   неисправности EEPROM физически никогда не отрисовывался. Запас
   до 100 — с учётом будущих иконок таймеров сна в инфозоне. */
static TextSlot_t g_slots[100];

/* Буфер для одной линии текста (ширина дисплея) */
static uint16_t scanline[ST7789_WIDTH];

/* ========================================================================== */
/* СИСТЕМНЫЕ ФУНКЦИИ                                                          */
/* ========================================================================== */

void DISPLAY_RegisterDriver(const Display_Driver_t *driver) { pDisp = driver; }
uint16_t DISPLAY_GetWidth(void)  { return pDisp ? pDisp->screen_width : 0; }
uint16_t DISPLAY_GetHeight(void) { return pDisp ? pDisp->screen_height : 0; }
bool DISPLAY_IsReady(void) { return (!pDisp || !pDisp->IsBusy) ? true : !pDisp->IsBusy(); }

/* ========================================================================== */
/* ГРАФИЧЕСКИЕ ПРИМИТИВЫ                                                      */
/* ========================================================================== */

void DISPLAY_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (pDisp && pDisp->FillRect) {
        while (!DISPLAY_IsReady());
        pDisp->FillRect(x, y, w, h, color);
    }
}

void DISPLAY_FillScreen(uint16_t color) {
    if (pDisp) DISPLAY_FillRect(0, 0, pDisp->screen_width, pDisp->screen_height, color);
}

void DISPLAY_DrawCircle(uint16_t x, uint16_t y, uint16_t r, uint16_t color) {
    if (pDisp && pDisp->DrawCircle) {
        while (!DISPLAY_IsReady());
        pDisp->DrawCircle(x, y, r, color);
    }
}

void DISPLAY_FillCircle(uint16_t x, uint16_t y, uint16_t r, uint16_t color) {
    if (pDisp && pDisp->FillCircle) {
        while (!DISPLAY_IsReady());
        pDisp->FillCircle(x, y, r, color);
    }
}

void DISPLAY_DrawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color) {
    if (pDisp && pDisp->DrawRect) {
        while (!DISPLAY_IsReady());
        pDisp->DrawRect(x, y, w, h, color);
    }
}

void DISPLAY_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color) {
    if (pDisp && pDisp->DrawVectorLine) {
        while (!DISPLAY_IsReady());
        pDisp->DrawVectorLine(x0, y0, x1, y1, color);
    }
}

/* ========================================================================== */
/* ТЕКСТ И UTF-8                                                              */
/* ========================================================================== */

/**
 * @brief Декодирует ОДИН символ UTF-8 в кодовую точку Unicode.
 *        Понимает только 1-байтные (ASCII) и 2-байтные
 *        последовательности — этого достаточно для латиницы+кириллицы,
 *        которые только и есть в шрифтах проекта (font->lut).
 *        3-байтные (например, большинство emoji, ряд других
 *        диапазонов) НЕ декодируются верно — попадают в else-ветку,
 *        которая просто пропускает один байт и возвращает 0
 *        (несуществующий глиф, DISPLAY_Print/GetTextWidth просто
 *        оставят зазор шириной font->height/3). Молча, без ошибки —
 *        если когда-нибудь понадобится реальный 3-байтный диапазон,
 *        эту функцию придётся дополнить.
 */
static uint16_t DecodeUTF8(const char **str) {
    const uint8_t *p = (const uint8_t*)*str;
    uint16_t unicode = 0;
    if ((*p & 0x80) == 0) {
        unicode = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
        unicode = (*p++ & 0x1F) << 6;
        unicode |= (*p++ & 0x3F);
    } else {
        p++;
    }
    *str = (const char*)p;
    return unicode;
}

static int16_t GetGlyphIndex(uint16_t unicode, const font_t *font) {
    if (!font || !font->lut) return -1;
    for (uint16_t i = 0; i < font->lut_size; i++) {
        if (font->lut[i] == unicode) return i;
    }
    return -1;
}

/**
 * @brief Отрисовка конкретного глифа в буфер линии и вывод на экран.
 * Изоляция этой логики снижает сложность DISPLAY_Print.
 */
static void DrawGlyph(uint16_t x, uint16_t y, int16_t idx, const font_t* font, uint16_t color, uint16_t bgcolor) {
    uint8_t width = font->widths[idx];
    int8_t xoff = font->xoffset[idx];
    int8_t yoff = font->yoffset[idx];
    uint8_t advance = font->dwidth[idx];
    uint32_t bitmap_off = font->offsets[idx];
    uint8_t bytes_per_col = (font->height + 7) / 8;

    if (advance > ST7789_WIDTH) return;

    for (uint8_t row = 0; row < font->height; row++) {
        for (uint8_t col = 0; col < advance; col++) {
            int16_t rel_col = col - xoff;
            int16_t rel_row = row - yoff;
            bool bit = false;

            if (rel_col >= 0 && rel_col < width && rel_row >= 0 && rel_row < font->height) {
                uint32_t col_off = bitmap_off + (uint32_t)rel_col * bytes_per_col;
                uint8_t byte = font->bitmap[col_off + (rel_row >> 3)];
                bit = byte & (1 << (7 - (rel_row & 7)));
            }
            scanline[col] = bit ? color : bgcolor;
        }
        while (!DISPLAY_IsReady());
        pDisp->DrawLine(x, y + row, advance, scanline);
    }
}

void DISPLAY_Print(uint16_t x, uint16_t y, const char* str, const font_t* font, uint16_t color, uint16_t bgcolor) {
    if (!pDisp || !str || !font) return;

    uint16_t cur_x = x;
    while (*str) {
        uint16_t unicode = DecodeUTF8(&str);
        int16_t idx = GetGlyphIndex(unicode, font);

        if (idx < 0) {
            cur_x += font->height / 3;
            continue;
        }

        DrawGlyph(cur_x, y, idx, font, color, bgcolor);
        cur_x += font->dwidth[idx];
    }
}

/* ========================================================================== */
/* SMART PRINT                                                                */
/* ========================================================================== */

void DISPLAY_ClearAllSlots(void) {
    memset(g_slots, 0, sizeof(g_slots));
}

void DISPLAY_ClearSlot(uint8_t slot) {
    if (slot < 100) memset(&g_slots[slot], 0, sizeof(TextSlot_t));
}

uint16_t DISPLAY_GetTextWidth(const char* str, const font_t* font) {
    if (!str || !font) return 0;
    uint16_t width = 0;
    const char* p = str;
    while (*p) {
        uint16_t unicode = DecodeUTF8(&p);
        int16_t idx = GetGlyphIndex(unicode, font);
        width += (idx >= 0) ? font->dwidth[idx] : (font->height / 3);
    }
    return width;
}

void DISPLAY_SmartPrint(uint8_t slot, uint16_t x, uint16_t y, const char* str, uint16_t color, uint16_t bgcolor, const font_t* font) {
    if (slot >= 100) return;

    // Если текст в слоте не изменился — выходим
    if (g_slots[slot].active && strcmp(g_slots[slot].text, str) == 0) return;

    // 1. Считаем ширину старого и нового текста
    uint16_t old_w = 0;
    if (g_slots[slot].active) {
        old_w = DISPLAY_GetTextWidth(g_slots[slot].text, font);
    }
    uint16_t new_w = DISPLAY_GetTextWidth(str, font);

    // 2. Если новый текст короче, затираем "хвост" прямоугольником фона
    if (old_w > new_w) {
        DISPLAY_FillRect(x + new_w, y, old_w - new_w, font->height, bgcolor);
    }

    // 3. Обновляем кэш слота
    strncpy(g_slots[slot].text, str, 47);
    g_slots[slot].text[47] = 0;
    g_slots[slot].active = true;

    // 4. Печатаем новый текст
    DISPLAY_Print(x, y, str, font, color, bgcolor);
}
