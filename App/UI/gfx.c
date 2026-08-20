/**
 * @file gfx.c
 * @brief Реализация текстовых примитивов. Формат битмапа шрифта — см.
 *        App/Fonts/bdf2c_TFT.py (генератор): столбцы сверху вниз, MSB-first,
 *        bytes_per_col = ceil(font->height / 8).
 *
 * Неблокирующая модель: Gfx_Process() продвигает текущее задание на один
 * символ за вызов и никогда не ждёт DMA — см. gfx.h.
 */

#include "gfx.h"
#include <string.h>

#define GFX_GLYPH_BUFFER_PIXELS (32 * 64)
static display_color_t s_glyph_buffer[GFX_GLYPH_BUFFER_PIXELS];


/* ---- Поиск глифа в шрифте (бинарный поиск по font->lut) ---- */

static int find_glyph_index(const font_t *font, uint32_t codepoint)
{
    int lo = 0, hi = font->lut_size - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t code = font->lut[mid];
        if (code == codepoint) return mid;
        if (code < codepoint) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1; /* Символ отсутствует в шрифте */
}

/* ---- Отрисовка одного символа: готовит буфер и ЗАПУСКАЕТ DMA (не ждёт) ---- */

static uint16_t submit_glyph(uint16_t x, uint16_t y, const font_t *font, int idx,
                              display_color_t fg, display_color_t bg)
{
    uint8_t  width        = font->widths[idx];
    int8_t   xoff          = font->xoffset[idx];
    int8_t   yoff          = font->yoffset[idx];
    uint8_t  advance       = font->dwidth[idx];
    uint16_t offset         = font->offsets[idx];
    uint8_t  bytes_per_col = (font->height + 7) / 8;

    if (width == 0) {
        return advance; /* Пробел и подобные — только сдвиг курсора */
    }

    uint32_t pixel_count = (uint32_t)width * font->height;
    if (pixel_count > GFX_GLYPH_BUFFER_PIXELS) {
        return advance; /* Символ не влезает в буфер — пропускаем отрисовку, но сдвигаем курсор */
    }

    for (uint16_t r = 0; r < font->height; r++) {
        for (uint16_t c = 0; c < width; c++) {
            const uint8_t *col_ptr = &font->bitmap[offset + (uint32_t)c * bytes_per_col];
            bool bit_set = (col_ptr[r / 8] & (0x80 >> (r % 8))) != 0;
            s_glyph_buffer[r * width + c] = bit_set ? fg : bg;
        }
    }

    uint16_t draw_x = x + xoff;
    uint16_t draw_y = y + yoff;

    if (Display_SetWindow(draw_x, draw_y, draw_x + width - 1, draw_y + font->height - 1) != DISPLAY_OK) {
        return advance;
    }
    Display_WritePixelsDMA(s_glyph_buffer, pixel_count); /* асинхронно, не ждём */

    return advance;
}

/* ---- Публичный интерфейс ---- */

Gfx_JobState_t Gfx_DrawTextStart(uint16_t x, uint16_t y, const char *text,
                                  const font_t *font,
                                  display_color_t fg_color, display_color_t bg_color)
{
    if (s_job.state == GFX_JOB_BUSY) {
        return GFX_JOB_BUSY; /* предыдущее задание ещё выполняется — отклоняем новое */
    }
    if (!text || !font) {
        return GFX_JOB_ERROR;
    }

    s_job.cursor = text;
    s_job.font   = font;
    s_job.fg     = fg_color;
    s_job.bg     = bg_color;
    s_job.x      = x;
    s_job.y      = y;
    s_job.state  = GFX_JOB_BUSY;

    return GFX_JOB_BUSY;
}

Gfx_JobState_t Gfx_Process(void)
{
    if (s_job.state != GFX_JOB_BUSY) {
        return s_job.state; /* IDLE или DONE — нечего делать */
    }
    if (Display_IsBusy()) {
        return GFX_JOB_BUSY; /* DMA ещё занят предыдущим символом — не ждём, выходим */
    }

    uint32_t cp = utf8_next(&s_job.cursor);
    if (cp == 0) {
        s_job.state = GFX_JOB_DONE;
        return GFX_JOB_DONE;
    }

    int idx = find_glyph_index(s_job.font, cp);
    if (idx >= 0) {
        s_job.x += submit_glyph(s_job.x, s_job.y, s_job.font, idx, s_job.fg, s_job.bg);
    }
    /* Символа нет в шрифте — просто пропускаем, курсор не двигаем */

    return GFX_JOB_BUSY; /* остались ещё символы (или узнаем на следующем вызове) */
}
