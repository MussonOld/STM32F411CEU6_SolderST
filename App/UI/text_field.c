#include "text_field.h"
#include "gfx.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    uint16_t x, y;
    const font_t *font;
    display_color_t fg, bg;
    char text[TEXTFIELD_LINE_TEXT_MAX];
    bool used;
    bool dirty;
} TextField_Line_t;

static TextField_Line_t s_lines[TEXTFIELD_MAX_LINES];

void TextField_Init(void)
{
    memset(s_lines, 0, sizeof(s_lines));
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
    /* Продвигаем текущее задание gfx (если есть) вне зависимости от того,
     * найдём ли ниже новую грязную строку — так символы дорисовываются
     * даже если в этом вызове стартовать новую отрисовку нельзя. */
    if (Gfx_Process() == GFX_JOB_BUSY) {
        return; /* gfx ещё занят предыдущим заданием — новое не начинаем */
    }

    for (uint8_t i = 0; i < TEXTFIELD_MAX_LINES; i++) {
        if (s_lines[i].used && s_lines[i].dirty) {
            Gfx_JobState_t st = Gfx_DrawTextStart(s_lines[i].x, s_lines[i].y,
                                                   s_lines[i].text, s_lines[i].font,
                                                   s_lines[i].fg, s_lines[i].bg);
            if (st != GFX_JOB_ERROR) {
                s_lines[i].dirty = false;
            }
            return; /* одна строка за вызов — не задерживаем главный цикл */
        }
    }
}
