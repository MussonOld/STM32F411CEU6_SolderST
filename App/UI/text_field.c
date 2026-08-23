#include "text_field.h"
#include "gfx.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    uint16_t x, y;
    const font_t *font;
    display_color_t fg, bg;
    char text[TEXTFIELD_LINE_TEXT_MAX];      /* желаемый текст (то, что должно быть показано) */
    char shown_text[TEXTFIELD_LINE_TEXT_MAX]; /* реально отображённый текст (для дозачистки хвоста) */
    bool used;
    bool dirty;
} TextField_Line_t;

static TextField_Line_t s_lines[TEXTFIELD_MAX_LINES];
static int s_active_line = -1; /* индекс строки с незавершённым заданием gfx, -1 если нет */

void TextField_Init(void)
{
    memset(s_lines, 0, sizeof(s_lines));
    s_active_line = -1;
}

void TextField_ConfigureLine(uint8_t line, uint16_t x, uint16_t y,
                        const font_t *font, display_color_t fg, display_color_t bg)
{
    if (line >= TEXTFIELD_MAX_LINES) {
        return;
    }
    s_lines[line].x = x;
    s_lines[line].y = y;
    s_lines[line].font = font;
    s_lines[line].fg = fg;
    s_lines[line].bg = bg;
    s_lines[line].text[0] = '\0';
    s_lines[line].shown_text[0] = '\0';
    s_lines[line].used = true;
    s_lines[line].dirty = false; /* пустая строка и так пуста на экране */
}

void TextField_Printf(uint8_t line, const char *fmt, ...)
{
    if (line >= TEXTFIELD_MAX_LINES || !s_lines[line].used) {
        return;
    }

    char buf[TEXTFIELD_LINE_TEXT_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (strncmp(buf, s_lines[line].text, TEXTFIELD_LINE_TEXT_MAX) != 0) {
        strncpy(s_lines[line].text, buf, TEXTFIELD_LINE_TEXT_MAX - 1);
        s_lines[line].text[TEXTFIELD_LINE_TEXT_MAX - 1] = '\0';
        s_lines[line].dirty = true;
        /* shown_text НЕ трогаем здесь — это то, что реально ещё на экране,
         * пригодится gfx.c для дозачистки хвоста, когда задание стартует. */
    }
}

void TextField_SetColors(uint8_t line, display_color_t fg, display_color_t bg)
{
    if (line >= TEXTFIELD_MAX_LINES || !s_lines[line].used) {
        return;
    }
    if (s_lines[line].fg == fg && s_lines[line].bg == bg) {
        return; /* цвета не изменились — перерисовка не нужна */
    }
    s_lines[line].fg = fg;
    s_lines[line].bg = bg;
    s_lines[line].dirty = true; /* принудительно, независимо от текста */
}

void TextField_Process(void)
{
    if (s_active_line >= 0) {
        Gfx_JobState_t st = Gfx_Process();
        if (st == GFX_JOB_BUSY) {
            return; /* задание текущей строки ещё выполняется (включая дозачистку хвоста) */
        }
        /* DONE (или ERROR — тоже считаем завершённым, не зацикливаемся) —
         * теперь то, что реально на экране, совпадает с text. */
        strncpy(s_lines[s_active_line].shown_text, s_lines[s_active_line].text,
                TEXTFIELD_LINE_TEXT_MAX - 1);
        s_lines[s_active_line].shown_text[TEXTFIELD_LINE_TEXT_MAX - 1] = '\0';
        s_active_line = -1;
        return; /* не начинаем новую строку в этом же вызове главного цикла */
    }

    for (uint8_t i = 0; i < TEXTFIELD_MAX_LINES; i++) {
        if (s_lines[i].used && s_lines[i].dirty) {
            Gfx_JobState_t st = Gfx_DrawTextStart(s_lines[i].x, s_lines[i].y,
                                                   s_lines[i].text, s_lines[i].shown_text,
                                                   s_lines[i].font,
                                                   s_lines[i].fg, s_lines[i].bg);
            s_lines[i].dirty = false;
            if (st == GFX_JOB_BUSY) {
                s_active_line = (int)i;
            }
            /* GFX_JOB_ERROR — задание не стартовало (например, font/text
             * некорректны); shown_text не обновляем, dirty уже снят —
             * решение то же, что было раньше: не зацикливаемся на ошибке. */
            return; /* одна строка за вызов — не задерживаем главный цикл */
        }
    }
}
