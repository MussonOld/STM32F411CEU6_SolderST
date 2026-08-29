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
 * файлы .c в App/Fonts (widths, xoffset, dwidth, height) и подними константу,
 * иначе в release часть символов будет заменяться видимым маркером
 * ошибки вместо реальной отрисовки, а в debug-сборке assert() уронит
 * сборку сразу же — не дожидаясь, пока баг обнаружится на экране. */
#define GFX_GLYPH_BUFFER_PIXELS (40 * 70)
static display_color_t s_glyph_buffer[GFX_GLYPH_BUFFER_PIXELS];

typedef struct {
    const char       *cursor;   /* текущая позиция в UTF-8 строке */
    const font_t      *font;
    display_color_t    fg, bg;
    uint16_t            x, y;    /* курсор отрисовки НОВОЙ строки */
    Gfx_JobState_t      state;
    uint32_t            erase_remaining_px;  /* >0, пока идёт чанкованное стирание старого прямоугольника (фаза ПЕРЕД отрисовкой) */
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

/**
 * @brief Отправить пиксели в Display_WritePixelsDMA() и учесть отказ запуска.
 *
 * DISPLAY_OK означает только то, что DMA-передача СТАРТОВАЛА (принята HAL) —
 * не то, что она гарантированно завершится успешно; асинхронное завершение
 * (успех/сбой) обрабатывает сам драйвер дисплея в HAL_SPI_TxCpltCallback()
 * (см. st7789.c) — оттуда s_busy корректно сбрасывается и при ошибке, GFX
 * этим не занимается и заниматься не должен.
 *
 * Проверяем "!= DISPLAY_OK", а не "== DISPLAY_ERROR": по контракту
 * DISPLAY_BUSY сюда дойти не должен (перед каждым submit_glyph()/чанком
 * стирания Gfx_Process() уже проверяет Display_IsBusy()), но если это
 * предположение когда-нибудь нарушится, GFX не должен продолжать работу с
 * ложным ощущением, что DMA запущен.
 *
 * При отказе переводит текущее задание в GFX_JOB_ERROR — дальнейшая
 * обработка задания останавливается на стороне вызывающего кода (см.
 * Gfx_Process()), не отправляем новые транши поверх уже неисправной шины.
 *
 * Та же модель (переход в GFX_JOB_ERROR при отказе) вручную повторена на
 * каждом вызове Display_SetWindow() ниже — исторически её отказ везде
 * молча проглатывался (символ просто не рисовался, курсор всё равно
 * двигался дальше, будто всё в порядке), пока это не поправили; отдельного
 * хелпера под неё нет — мест немного, инлайн-проверка нагляднее.
 *
 * @return true — DMA запущен, false — отказ (s_job.state уже выставлен)
 */
static bool write_pixels_or_fail(const display_color_t *data, uint32_t count)
{
    Display_Status_t status = Display_WritePixelsDMA(data, count);
    if (status != DISPLAY_OK) {
        s_job.state = GFX_JOB_ERROR;
        return false;
    }
    return true;
}

/* Размер аварийного маркера "символ не влез в буфер" (см. release-ветки
 * в submit_glyph() ниже) — заведомо на порядки меньше GFX_GLYPH_BUFFER_PIXELS,
 * чтобы влезать в буфер вообще при любых разумных значениях константы. */
#define GFX_OVERFLOW_MARKER_SIZE_PX (6U)

/**
 * @brief Нарисовать маленький сплошной маркер вместо символа, который
 *        физически не влез в s_glyph_buffer.
 *
 * Используется ТОЛЬКО в release-сборке (в debug соответствующая ветка
 * роняет сборку через assert() раньше, чем доходит сюда) — как защита от
 * будущего рассинхрона GFX_GLYPH_BUFFER_PIXELS с реальными требованиями
 * шрифтов (см. историю бага в докстринге у GFX_GLYPH_BUFFER_PIXELS). Тихий
 * пропуск символа выглядит как ПРАВДОПОДОБНОЕ, но неверное число (особенно
 * опасно для Comic_60_dig — цифр температуры: "50" вместо "450" не похоже
 * на поломку). Явный маркер вместо этого — видимый артефакт, который
 * оператор не спутает с настоящим показанием.
 */
static void draw_glyph_overflow_marker(uint16_t x, uint16_t y, uint16_t advance,
                                        uint16_t height, display_color_t fg)
{
    uint16_t w = (advance < GFX_OVERFLOW_MARKER_SIZE_PX) ? advance : GFX_OVERFLOW_MARKER_SIZE_PX;
    uint16_t h = (height  < GFX_OVERFLOW_MARKER_SIZE_PX) ? height  : GFX_OVERFLOW_MARKER_SIZE_PX;
    if (w == 0 || h == 0) {
        return; /* нулевой advance/height — рисовать нечего, курсор всё равно сдвинется в вызывающем коде */
    }

    uint32_t px_count = (uint32_t)w * h; /* <= GFX_OVERFLOW_MARKER_SIZE_PX^2, заведомо << GFX_GLYPH_BUFFER_PIXELS */
    for (uint32_t i = 0; i < px_count; i++) {
        s_glyph_buffer[i] = fg; /* сплошной маркер — не пытаемся имитировать форму символа */
    }
    if (Display_SetWindow(x, y, x + w - 1, y + h - 1) != DISPLAY_OK) {
        s_job.state = GFX_JOB_ERROR;
        return; /* отказ — не рисуем; состояние увидит Gfx_Process() у вызывающего submit_glyph() */
    }
    write_pixels_or_fail(s_glyph_buffer, px_count);
}

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
            assert(px_count <= GFX_GLYPH_BUFFER_PIXELS &&
                   "Glyph cell exceeds GFX_GLYPH_BUFFER_PIXELS - increase the buffer");
            draw_glyph_overflow_marker(x, y, advance, font->height, fg);
            return advance;
        }
        for (uint32_t i = 0; i < px_count; i++) {
            s_glyph_buffer[i] = bg;
        }
        if (Display_SetWindow(x, y, x + advance - 1, y + font->height - 1) != DISPLAY_OK) {
            s_job.state = GFX_JOB_ERROR;
            return advance; /* курсор всё равно двигаем; Gfx_Process() проверит состояние сразу после возврата */
        }
        write_pixels_or_fail(s_glyph_buffer, px_count);
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
        /* В debug (DEBUG определён CubeIDE по умолчанию) роняем сборку
         * сразу здесь — иначе баг обнаруживается только на экране,
         * произвольно поздно, как это уже дважды случалось с cell_width
         * у Comic_60_dig. В release assert отключён (NDEBUG) — раньше
         * это был тихий пропуск (курсор сдвигается, глиф не рисуется),
         * что для показаний температуры опасно: результат выглядит как
         * ПРАВДОПОДОБНОЕ, но неверное число ("50" вместо "450"), а не
         * как очевидная поломка. Вместо тихого пропуска — видимый
         * маркер ошибки, заведомо укладывающийся в буфер. */
        assert(pixel_count <= GFX_GLYPH_BUFFER_PIXELS &&
               "Glyph cell exceeds GFX_GLYPH_BUFFER_PIXELS - increase the buffer");
        draw_glyph_overflow_marker(x, y, advance, font->height, fg);
        return advance; /* курсор всё равно сдвигаем на advance, как и раньше */
    }

    for (uint32_t i = 0; i < pixel_count; i++) {
        s_glyph_buffer[i] = bg; /* фон по всей ячейке — стираем всё, что было раньше */
    }

    /* Окно отрисовки/стирания по Y ФИКСИРОВАНО на y..y+height-1 для ЛЮБОГО
     * символа шрифта — не должно зависеть от yoffset конкретного глифа.
     * yoff — это позиция битмапа символа ВНУТРИ этого фиксированного окна
     * (строка буфера = r, битмап-строка = r - yoff), а не сдвиг самого окна.
     * Если окно сдвигать вместе с yoff (как было раньше), то при смене
     * символа с меньшим yoff (у "4" среди цифр он на 1 меньше, чем у
     * остальных) на символ с большим yoff верхние строки старого глифа
     * остаются вне нового окна стирания — "призрак" сверху. */
    uint16_t glyph_col0 = (uint16_t)(xoff - cell_left);
    for (uint16_t r = 0; r < font->height; r++) {
        int16_t bmp_row = (int16_t)r - yoff;
        if (bmp_row < 0 || bmp_row >= font->height) {
            continue; /* вне битмапа символа — остаётся фон, уже залит выше */
        }
        for (uint16_t c = 0; c < width; c++) {
            const uint8_t *col_ptr = &font->bitmap[offset + (uint32_t)c * bytes_per_col];
            bool bit_set = (col_ptr[bmp_row / 8] & (0x80 >> (bmp_row % 8))) != 0;
            if (bit_set) {
                s_glyph_buffer[r * cell_width + glyph_col0 + c] = fg;
            }
        }
    }

    uint16_t draw_x = x + cell_left;
    uint16_t draw_y = y;

    if (Display_SetWindow(draw_x, draw_y, draw_x + cell_width - 1, draw_y + font->height - 1) != DISPLAY_OK) {
        s_job.state = GFX_JOB_ERROR;
        return advance; /* курсор всё равно двигаем; Gfx_Process() проверит состояние сразу после возврата */
    }
    write_pixels_or_fail(s_glyph_buffer, pixel_count); /* асинхронно, не ждём; отказ запуска — см. write_pixels_or_fail() */

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

/**
 * @brief Реальный правый край текста, если бы он был нарисован с курсора
 *        x=0 — то есть максимум по ВСЕМ символам от (накопленный advance
 *        до символа + cell_right ЭТОГО символа), а не просто сумма advance
 *        (см. Gfx_MeasureTextWidth()).
 *
 * У большинства глифов "чернила" укладываются в свой advance (xoff+width
 * <= advance), и тогда это совпадает с Gfx_MeasureTextWidth(). Но у
 * некоторых (например "4" в AntiquaB_18_uni: width=10, xoff=0, advance=9)
 * штрих на 1 столбец шире собственного advance — submit_glyph() рисует
 * этот столбец (её cell_right = max(xoff+width, advance)), физически он
 * оказывается на экране, а Gfx_MeasureTextWidth() (сумма advance) его не
 * учитывает. Нужна ИМЕННО эта функция — не Gfx_MeasureTextWidth() — там,
 * где ширина текста определяет размер прямоугольника СТИРАНИЯ (см.
 * Gfx_DrawTextStart()): иначе прямоугольник стирания на 1px уже реально
 * нарисованного, и после такого символа остаётся столбец-призрак (не
 * стирается никогда, пока какой-то другой символ не перекроет ту же
 * позицию). Для позиционирования (центровка/выравнивание — где нужен
 * именно "курсорный" типографский шаг, а не bbox чернил) по-прежнему
 * нужна Gfx_MeasureTextWidth(), не эта функция.
 */
static uint16_t measure_real_right_edge(const font_t *font, const char *text)
{
    uint16_t cursor_x = 0;
    uint16_t max_right = 0;
    const char *cursor = text;

    for (;;) {
        uint32_t cp = utf8_next(&cursor);
        if (cp == 0) {
            break;
        }
        int idx = find_glyph_index(font, cp);
        if (idx < 0) {
            continue; /* см. Gfx_MeasureTextWidth() — курсор не двигаем */
        }
        uint8_t  width   = font->widths[idx];
        int8_t   xoff    = font->xoffset[idx];
        uint8_t  advance = font->dwidth[idx];

        /* Та же формула, что и cell_right в submit_glyph() — реальный
         * правый край ЭТОГО символа относительно его собственного x. */
        int16_t cell_right = (width == 0) ? (int16_t)advance
                            : (((int16_t)xoff + width > advance) ? ((int16_t)xoff + width) : advance);
        uint16_t right_edge = (uint16_t)(cursor_x + cell_right);
        if (right_edge > max_right) {
            max_right = right_edge;
        }
        cursor_x = (uint16_t)(cursor_x + advance);
    }

    return max_right;
}

Gfx_JobState_t Gfx_DrawTextStart(uint16_t x, uint16_t y, const char *text,
                                  uint16_t prev_x, const char *prev_text,
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
    s_job.erase_remaining_px = 0;

    if (prev_text && prev_text[0] != '\0') {
        /* Не Gfx_MeasureTextWidth() — нужен реальный "чернильный" правый
         * край, а не сумма advance, иначе после символов с overhang
         * (см. measure_real_right_edge()) остаётся столбец-призрак. */
        uint16_t old_w = measure_real_right_edge(font, prev_text);

        /* Реально нарисованная (и потому подлежащая стиранию) ширина не
         * может выходить за правый край экрана — submit_glyph() тихо
         * пропускает отрисовку символов за границей (см. там), так что
         * физически на экране никогда не было пикселей старого текста
         * правее последнего столбца. Без этой отсечки Display_SetWindow()
         * ниже получал x1 >= ширина экрана на длинных строках (например,
         * предупреждение Expert) и возвращал DISPLAY_ERROR — весь блок
         * стирания молча пропускался целиком, оставляя "хвосты" старого
         * текста на экране НАВСЕГДА (до следующей перерисовки этой же
         * строки поверх того же места). */
        uint16_t screen_w;
        Display_GetSize(&screen_w, NULL);
        uint16_t max_w = (prev_x < screen_w) ? (uint16_t)(screen_w - prev_x) : 0;
        if (old_w > max_w) {
            old_w = max_w;
        }

        if (old_w > 0) {
            /* Стираем СТАРЫЙ прямоугольник целиком, по СТАРОМУ x (prev_x),
             * ДО отрисовки новой строки — покрывает и рост/сокращение
             * ширины, и смену позиции (перецентровку) одним механизмом,
             * без частных случаев. Небольшая избыточность (при перекрытии
             * старого и нового прямоугольников часть пикселей стирается и
             * тут же перерисовывается заново) — цена за простоту и
             * корректность во всех случаях сразу. */
            if (Display_SetWindow(prev_x, y, prev_x + old_w - 1, y + font->height - 1) != DISPLAY_OK) {
                s_job.state = GFX_JOB_ERROR; /* раньше здесь молча пропускалось стирание и функция всё равно возвращала GFX_JOB_BUSY */
            } else {
                s_job.erase_remaining_px = (uint32_t)old_w * font->height;
            }
        }
    }

    return s_job.state; /* GFX_JOB_BUSY, либо GFX_JOB_ERROR при отказе SetWindow выше */
}

Gfx_JobState_t Gfx_Process(void)
{
    if (s_job.state != GFX_JOB_BUSY) {
        return s_job.state; /* IDLE или DONE — нечего делать */
    }
    if (Display_IsBusy()) {
        return GFX_JOB_BUSY; /* DMA ещё занят предыдущим шагом — не ждём, выходим */
    }

    /* Фаза стирания старого прямоугольника — идёт ПЕРВОЙ, до посимвольного
     * прохода новой строки (см. окно, уже установленное в DrawTextStart()).
     * Чанкуем по GFX_GLYPH_BUFFER_PIXELS, чтобы не переполнить статический
     * буфер на крупных шрифтах (например Comic_60_dig) и не слать в DMA
     * новый чанк, пока предыдущий ещё не ушёл — Display_IsBusy() проверяется
     * выше на каждый вызов, так что чанки естественным образом растягиваются
     * по нескольким вызовам Gfx_Process(), не блокируя главный цикл. */
    if (s_job.erase_remaining_px > 0) {
        uint32_t chunk = (s_job.erase_remaining_px > GFX_GLYPH_BUFFER_PIXELS)
                          ? GFX_GLYPH_BUFFER_PIXELS : s_job.erase_remaining_px;
        for (uint32_t i = 0; i < chunk; i++) {
            s_glyph_buffer[i] = s_job.bg;
        }
        if (!write_pixels_or_fail(s_glyph_buffer, chunk)) {
            return GFX_JOB_ERROR; /* DMA не стартовал — remaining НЕ трогаем, задание уже помечено ERROR */
        }
        s_job.erase_remaining_px -= chunk;
        return GFX_JOB_BUSY; /* даже когда дошли до 0 — следующий вызов уйдёт в посимвольный проход ниже */
    }

    uint32_t cp = utf8_next(&s_job.cursor);
    if (cp == 0) {
        s_job.state = GFX_JOB_DONE;
        return GFX_JOB_DONE;
    }

    int idx = find_glyph_index(s_job.font, cp);
    if (idx >= 0) {
        uint16_t advance = submit_glyph(s_job.x, s_job.y, s_job.font, idx, s_job.fg, s_job.bg);
        s_job.x += advance;
        if (s_job.state == GFX_JOB_ERROR) {
            return GFX_JOB_ERROR; /* DMA не стартовал внутри submit_glyph() — не продолжаем строку */
        }
    }
    /* Символа нет в шрифте — просто пропускаем, курсор не двигаем */

    return GFX_JOB_BUSY; /* остались ещё символы (или узнаем на следующем вызове) */
}

void Gfx_CancelJob(void)
{
    if (s_job.state != GFX_JOB_BUSY) {
        return; /* нечего отменять */
    }
    while (Display_IsBusy()) { } /* см. докстринг в gfx.h — не наступаем на буфер/шину DMA-в-полёте */
    s_job.state = GFX_JOB_IDLE;
    s_job.erase_remaining_px = 0;
}
