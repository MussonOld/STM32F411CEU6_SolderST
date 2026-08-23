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
#include <assert.h>

/* Размер буфера под один глиф/ячейку перерисовки в пикселях.
 * Должен покрывать (cell_width * height) самой требовательной комбинации
 * среди ВСЕХ используемых шрифтов — см. submit_glyph(): cell_width
 * объединяет bbox символа (xoff..xoff+width) и шаг курсора (advance),
 * поэтому считать нужно именно по этой формуле, а не по одной ширине
 * width. На момент правки максимум даёт Comic_60_dig (height=67,
 * cell_width=37 у большинства цифр из-за advance=37 -> 2479 пикселей).
 * Это УЖЕ ВТОРОЙ раз, когда эта константа отстаёт от реальных требований
 * шрифтов (см. историю: сначала ловили это на '4'/'7'/'9', затем формула
 * cell_width расширилась и лимит стал слишком мал почти для всех цифр).
 * Если меняешь шрифты/формулу ячейки — пересчитай через
 * App/Fonts/.c (widths, xoffset, dwidth, height) и подними константу,
 * иначе часть символов будет молча пропадать (см. assert() ниже —
 * он поймает это хотя бы в debug-сборке, а не только на экране). */
#define GFX_GLYPH_BUFFER_PIXELS (40 * 70)
static display_color_t s_glyph_buffer[GFX_GLYPH_BUFFER_PIXELS];

typedef struct {
    const char       *cursor;   /* текущая позиция в UTF-8 строке */
    const font_t      *font;
    display_color_t    fg, bg;
    uint16_t            x, y;    /* текущий курсор отрисовки */
    Gfx_JobState_t      state;
    uint16_t            erase_to_x;         /* дозачистить хвост до этого x (эксклюзивно) */
    bool                erase_pending;       /* true — после конца новой строки нужна дозачистка хвоста */
    uint32_t            erase_remaining_px;  /* >0, пока идёт чанкованная заливка хвоста */
} gfx_job_t;

static gfx_job_t s_job = { .state = GFX_JOB_IDLE };

/* ---- UTF-8 декодирование ---- */

static uint32_t utf8_next(const char **str)
{
    const uint8_t *s = (const uint8_t *)*str;
    if (*s == 0) return 0;

    uint32_t cp;
    int extra;

    if ((*s & 0x80) == 0x00)      { cp = *s;        extra = 0; }
    else if ((*s & 0xE0) == 0xC0) { cp = *s & 0x1F;  extra = 1; }
    else if ((*s & 0xF0) == 0xE0) { cp = *s & 0x0F;  extra = 2; }
    else if ((*s & 0xF8) == 0xF0) { cp = *s & 0x07;  extra = 3; }
    else { *str = (const char *)(s + 1); return 0xFFFD; /* невалидный байт */ }

    s++;
    for (int i = 0; i < extra; i++) {
        if ((*s & 0xC0) != 0x80) { *str = (const char *)s; return 0xFFFD; }
        cp = (cp << 6) | (*s & 0x3F);
        s++;
    }

    *str = (const char *)s;
    return cp;
}

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
        /* Пробел и подобные не имеют своего битмапа, но должны стереть
         * то, что было на этом месте раньше (иначе при сокращении числа
         * цифр остаётся "призрак" старого символа) — заливаем ячейку bg. */
        if (advance == 0) {
            return advance;
        }
        uint32_t px_count = (uint32_t)advance * font->height;
        if (px_count > GFX_GLYPH_BUFFER_PIXELS) {
            return advance;
        }
        for (uint32_t i = 0; i < px_count; i++) {
            s_glyph_buffer[i] = bg;
        }
        if (Display_SetWindow(x, y, x + advance - 1, y + font->height - 1) == DISPLAY_OK) {
            Display_WritePixelsDMA(s_glyph_buffer, px_count);
        }
        return advance;
    }

    /* Ячейка перерисовки = объединение bbox символа (xoff..xoff+width) и
     * шага курсора (0..advance). Пиксели предыдущего символа на этой позиции
     * могли выходить за bbox нового (например широкая "0", затем узкая "1"
     * с другим xoff) — если стирать только bbox нового символа, часть
     * старых пикселей остаётся ("призрак"). Стирая всю ячейку целиком,
     * гарантируем перекрытие независимо от формы предыдущего символа. */
    int16_t  cell_left  = (xoff < 0) ? xoff : 0;
    int16_t  cell_right = ((int16_t)xoff + width > advance) ? ((int16_t)xoff + width) : advance;
    uint16_t cell_width = (uint16_t)(cell_right - cell_left);

    uint32_t pixel_count = (uint32_t)cell_width * font->height;
    if (pixel_count > GFX_GLYPH_BUFFER_PIXELS) {
        /* В release это по-прежнему тихий пропуск (курсор сдвигается,
         * но глиф/ячейка не рисуется) — так проект собирается и без
         * правки буфера, просто визуально теряет самые требовательные
         * символы. В debug (DEBUG определён CubeIDE по умолчанию) роняем
         * сборку сразу здесь — иначе баг обнаруживается только на экране,
         * произвольно поздно, как это уже дважды случалось с cell_width
         * у Comic_60_dig. */
        assert(pixel_count <= GFX_GLYPH_BUFFER_PIXELS &&
               "Glyph cell exceeds GFX_GLYPH_BUFFER_PIXELS - increase the buffer");
        return advance; /* Символ не влезает в буфер — пропускаем отрисовку, но сдвигаем курсор */
    }

    for (uint32_t i = 0; i < pixel_count; i++) {
        s_glyph_buffer[i] = bg; /* фон по всей ячейке — стираем всё, что было раньше */
    }

    uint16_t glyph_col0 = (uint16_t)(xoff - cell_left);
    for (uint16_t r = 0; r < font->height; r++) {
        for (uint16_t c = 0; c < width; c++) {
            const uint8_t *col_ptr = &font->bitmap[offset + (uint32_t)c * bytes_per_col];
            bool bit_set = (col_ptr[r / 8] & (0x80 >> (r % 8))) != 0;
            if (bit_set) {
                s_glyph_buffer[r * cell_width + glyph_col0 + c] = fg;
            }
        }
    }

    uint16_t draw_x = x + cell_left;
    uint16_t draw_y = y + yoff;

    if (Display_SetWindow(draw_x, draw_y, draw_x + cell_width - 1, draw_y + font->height - 1) != DISPLAY_OK) {
        return advance;
    }
    Display_WritePixelsDMA(s_glyph_buffer, pixel_count); /* асинхронно, не ждём */

    return advance;
}

/* ---- Публичный интерфейс ---- */

uint16_t Gfx_MeasureTextWidth(const font_t *font, const char *text)
{
    if (!font || !text) {
        return 0;
    }

    uint16_t width = 0;
    const char *cursor = text;

    for (;;) {
        uint32_t cp = utf8_next(&cursor);
        if (cp == 0) {
            break;
        }
        int idx = find_glyph_index(font, cp);
        if (idx >= 0) {
            width = (uint16_t)(width + font->dwidth[idx]);
        }
        /* Символа нет в шрифте — пропускаем, ширину не добавляем (как и
         * сам Gfx_Process() не двигает курсор для отсутствующих глифов) */
    }

    return width;
}

Gfx_JobState_t Gfx_DrawTextStart(uint16_t x, uint16_t y, const char *text,
                                  const char *prev_text, const font_t *font,
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
    s_job.erase_pending = false;
    s_job.erase_remaining_px = 0;

    if (prev_text && prev_text[0] != '\0') {
        uint16_t new_w = Gfx_MeasureTextWidth(font, text);
        uint16_t old_w = Gfx_MeasureTextWidth(font, prev_text);
        if (old_w > new_w) {
            s_job.erase_to_x = (uint16_t)(x + old_w);
            s_job.erase_pending = true;
        }
    }

    return GFX_JOB_BUSY;
}

Gfx_JobState_t Gfx_Process(void)
{
    if (s_job.state != GFX_JOB_BUSY) {
        return s_job.state; /* IDLE или DONE — нечего делать */
    }
    if (Display_IsBusy()) {
        return GFX_JOB_BUSY; /* DMA ещё занят предыдущим шагом — не ждём, выходим */
    }

    /* Фаза дозачистки хвоста — идёт ПОСЛЕ обычного посимвольного прохода
     * (см. ветку cp==0 ниже). Чанкуем по GFX_GLYPH_BUFFER_PIXELS, чтобы не
     * переполнить статический буфер на крупных шрифтах (например
     * Comic_60_dig) и не слать в DMA новый чанк, пока предыдущий ещё не
     * ушёл — Display_IsBusy() проверяется выше на каждый вызов, так что
     * чанки естественным образом растягиваются по нескольким вызовам
     * Gfx_Process(), не блокируя главный цикл. */
    if (s_job.erase_remaining_px > 0) {
        uint32_t chunk = (s_job.erase_remaining_px > GFX_GLYPH_BUFFER_PIXELS)
                          ? GFX_GLYPH_BUFFER_PIXELS : s_job.erase_remaining_px;
        for (uint32_t i = 0; i < chunk; i++) {
            s_glyph_buffer[i] = s_job.bg;
        }
        Display_WritePixelsDMA(s_glyph_buffer, chunk);
        s_job.erase_remaining_px -= chunk;
        if (s_job.erase_remaining_px == 0) {
            s_job.state = GFX_JOB_DONE;
            return GFX_JOB_DONE;
        }
        return GFX_JOB_BUSY;
    }

    uint32_t cp = utf8_next(&s_job.cursor);
    if (cp == 0) {
        if (s_job.erase_pending) {
            s_job.erase_pending = false;
            /* s_job.x здесь — фактическая ширина отрисованной новой строки
             * (учитывает пропущенные глифы), не предварительная оценка. */
            if (s_job.erase_to_x > s_job.x) {
                uint16_t erase_width = (uint16_t)(s_job.erase_to_x - s_job.x);
                if (Display_SetWindow(s_job.x, s_job.y, s_job.erase_to_x - 1,
                                       s_job.y + s_job.font->height - 1) == DISPLAY_OK) {
                    s_job.erase_remaining_px = (uint32_t)erase_width * s_job.font->height;
                    return GFX_JOB_BUSY; /* первый чанк дозачистки уйдёт на следующем вызове */
                }
            }
        }
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
