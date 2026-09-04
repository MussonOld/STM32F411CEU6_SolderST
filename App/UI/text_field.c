#include "text_field.h"
#include "gfx.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef struct {
    uint16_t x, y;
    uint16_t shown_x;                         /* x, по которому реально нарисован shown_text */
    const font_t *font;
    display_color_t fg, bg;
    char text[TEXTFIELD_LINE_TEXT_MAX];       /* желаемый текст (то, что должно быть показано) */
    char shown_text[TEXTFIELD_LINE_TEXT_MAX]; /* реально отображённый текст (для стирания старого прямоугольника) */
    bool used;
    bool dirty;
} TextField_Line_t;

static TextField_Line_t s_lines[TEXTFIELD_MAX_LINES];
static int s_active_line = -1; /* индекс строки с незавершённым заданием gfx, -1 если нет */

/* Снапшот того, что РЕАЛЬНО передано в Gfx_DrawTextStart() для активной
 * строки — текст и x на момент старта задания. НЕ читать s_lines[i].text/x
 * напрямую, пока задание в работе: gfx.c держит s_job.cursor прямо на
 * буфер, который сюда передан, и растягивает его чтение на много вызовов
 * Gfx_Process() (по одному символу за вызов). Если в это время придёт
 * TextField_Printf() для ТОЙ ЖЕ строки (для температуры — реалистично,
 * State обновляется независимо от завершения DMA), set_line_text() перепишет
 * s_lines[i].text прямо посреди чтения — s_job.cursor может уйти на уже
 * невалидную позицию (при укорачивании текста — досрочный '\0', обрезанный
 * рендер с "мусором" на экране, см. историю бага). Раз активна максимум
 * одна строка одновременно (один s_active_line), одного снапшота хватает. */
static char     s_active_text[TEXTFIELD_LINE_TEXT_MAX];
static uint16_t s_active_x;

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
    s_lines[line].shown_x = x;
    s_lines[line].font = font;
    s_lines[line].fg = fg;
    s_lines[line].bg = bg;
    s_lines[line].text[0] = '\0';
    s_lines[line].shown_text[0] = '\0';
    s_lines[line].used = true;
    s_lines[line].dirty = false; /* пустая строка и так пуста на экране */
}

/**
 * @brief Общая часть Printf/PrintfCentered/PrintfRightAligned: применяет уже
 *        отформатированный текст — обновляет x, взводит dirty при реальном
 *        изменении. Bounds-check строки — на стороне вызывающих публичных
 *        функций (там же он нужен ДО форматирования/замера ширины, так что
 *        повторять его здесь было бы избыточно).
 */
static void set_line_text(uint8_t line, uint16_t new_x, const char *text)
{
    s_lines[line].x = new_x; /* дёшево пересчитывать каждый раз, даже если текст не изменился */

    if (strncmp(text, s_lines[line].text, TEXTFIELD_LINE_TEXT_MAX) != 0) {
        strncpy(s_lines[line].text, text, TEXTFIELD_LINE_TEXT_MAX - 1);
        s_lines[line].text[TEXTFIELD_LINE_TEXT_MAX - 1] = '\0';
        s_lines[line].dirty = true;
        /* shown_text/shown_x НЕ трогаем здесь — это то, что реально ещё на
         * экране, пригодится gfx.c для стирания старого прямоугольника,
         * когда задание стартует. */
    }
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

    set_line_text(line, s_lines[line].x, buf); /* x не меняется — обычное поведение */
}

void TextField_PrintfCentered(uint8_t line, uint16_t center_x, const char *fmt, ...)
{
    if (line >= TEXTFIELD_MAX_LINES || !s_lines[line].used) {
        return;
    }

    char buf[TEXTFIELD_LINE_TEXT_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uint16_t width = Gfx_MeasureTextWidth(s_lines[line].font, buf);
    uint16_t half = (uint16_t)(width / 2);
    uint16_t new_x = (center_x > half) ? (uint16_t)(center_x - half) : 0;

    set_line_text(line, new_x, buf);
}

void TextField_PrintfRightAligned(uint8_t line, uint16_t right_edge_x, const char *fmt, ...)
{
    if (line >= TEXTFIELD_MAX_LINES || !s_lines[line].used) {
        return;
    }

    char buf[TEXTFIELD_LINE_TEXT_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    uint16_t width = Gfx_MeasureTextWidth(s_lines[line].font, buf);
    uint16_t new_x = (right_edge_x > width) ? (uint16_t)(right_edge_x - width) : 0;

    set_line_text(line, new_x, buf);
}

uint16_t TextField_GetShownWidth(uint8_t line)
{
    if (line >= TEXTFIELD_MAX_LINES || !s_lines[line].used) {
        return 0;
    }
    /* Ширина именно shown_text (что физически ещё на экране), не text
     * (желаемое) — см. докстринг в text_field.h. Тем же измерением
     * (Gfx_MeasureTextWidth) пользуется и gfx.c при расчёте прямоугольника
     * стирания в Gfx_DrawTextStart(), так что значение совпадает 1-в-1 с
     * тем, что реально будет стёрто. */
    return Gfx_MeasureTextWidth(s_lines[line].font, s_lines[line].shown_text);
}

bool TextField_IsSettled(uint8_t line)
{
    if (line >= TEXTFIELD_MAX_LINES || !s_lines[line].used) {
        return true; /* нет такой строки — трогать нечего, не блокируем вызывающий код */
    }
    /* Пока задание для строки не завершилось, text и shown_text различаются
     * (см. TextField_Process()/set_line_text()) — как только gfx.c доигрывает
     * задание до конца, TextField_Process() копирует снапшот в shown_text
     * и они снова совпадают. Это тот же критерий, что использует сам
     * TextField_Process() (через сравнение при старте нового задания), так
     * что "settled" здесь означает буквально "ничего не рисуется/не ждёт
     * рисования для этой строки прямо сейчас". */
    return strncmp(s_lines[line].text, s_lines[line].shown_text, TEXTFIELD_LINE_TEXT_MAX) == 0;
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

void TextField_InvalidateAll(void)
{
    for (uint8_t i = 0; i < TEXTFIELD_MAX_LINES; i++) {
        if (s_lines[i].used) {
            /* Обнуляем И text, И shown_text — не только "что реально
             * нарисовано", но и "что должно быть нарисовано". Иначе строки,
             * принадлежащие экрану, который сейчас НЕ обновляется (например,
             * поля главного экрана, пока активно сервисное меню — их
             * Screen_Update() больше не трогает, render_menu() занят только
             * своими строками), всё равно останутся dirty=true со СТАРЫМ
             * содержимым в text и будут перерисованы TextField_Process()
             * поверх уже стёртого фона — то самое наложение экранов.
             * Опустошив text, они честно "ничего не показывают", пока их
             * снова не позовут через TextField_Printf(). */
            s_lines[i].text[0] = '\0';
            s_lines[i].shown_text[0] = '\0';
            s_lines[i].dirty = true;
        }
    }
    if (s_active_line >= 0) {
        /* Незавершённое задание в gfx.c принадлежало именно этой строке —
         * оно больше не актуально, и после сброса s_active_line ниже мы
         * никогда больше не позовём Gfx_Process() для него. Без явной
         * отмены s_job в gfx.c навсегда останется в GFX_JOB_BUSY, и
         * ЛЮБОЙ следующий Gfx_DrawTextStart() (для любой строки — задание
         * в gfx.c общее на весь модуль) будет отклоняться по кругу —
         * экран перестаёт обновляться насовсем. */
        Gfx_CancelJob();
    }
    s_active_line = -1; /* экран стёрт целиком снаружи — любое недорисованное задание уже неактуально */
}

void TextField_Process(void)
{
    if (s_active_line >= 0) {
        Gfx_JobState_t st = Gfx_Process();
        if (st == GFX_JOB_BUSY) {
            return; /* задание текущей строки ещё выполняется (включая стирание старого прямоугольника) */
        }
        if (st == GFX_JOB_ERROR_RETRY) {
            /* Транзиентный отказ уровня железа/шины (SetWindow/DMA — см.
             * gfx.c) на одном из чанков/символов посреди уже идущего
             * рендера — на экране сейчас неизвестно что: возможно часть
             * НОВОГО текста уже нарисована поверх части СТАРОГО
             * прямоугольника стирания. НЕЛЬЗЯ считать s_active_text
             * показанным (в отличие от настоящего DONE) — иначе shown_text
             * разойдётся с реальным содержимым экрана НАВСЕГДА (следующее
             * стирание будет мерить несуществующий прямоугольник).
             * Оставляем shown_text/shown_x как были (последнее ДОСТОВЕРНО
             * отрисованное состояние до этой попытки) и повторно взводим
             * dirty — на следующей итерации главного цикла строка
             * перерисуется с нуля тем же (или уже новым, если успели
             * прийти новые данные) текстом. */
            s_lines[s_active_line].dirty = true;
            s_active_line = -1;
            return;
        }
        if (st == GFX_JOB_ERROR_FATAL) {
            /* На практике сюда не попадаем: Gfx_Process() продвигает уже
             * ПОСТАВЛЕННОЕ задание — единственный источник ERROR_FATAL
             * (неверные text/font) проверяется раньше, в
             * Gfx_DrawTextStart(), и такое задание никогда не становится
             * s_active_line (см. ниже). Ветка — только чтобы switch по
             * Gfx_JobState_t был исчерпывающим и не молчал при будущих
             * изменениях gfx.c; ретраить не пытаемся — баг конфигурации
             * сам себя не починит. */
            s_active_line = -1;
            return;
        }
        /* GFX_JOB_DONE — задание реально дорисовано целиком, shown_text/
         * shown_x берём из СНАПШОТА (что реально ушло в gfx.c и физически
         * нарисовано), а не из s_lines[...].text/x — те могли уже
         * измениться за время рендера (см. комментарий у s_active_text
         * выше). Если изменились — dirty уже выставлен set_line_text(), эта же
         * строка переотрисуется на следующем заходе с уже новым значением. */
        strncpy(s_lines[s_active_line].shown_text, s_active_text, TEXTFIELD_LINE_TEXT_MAX - 1);
        s_lines[s_active_line].shown_text[TEXTFIELD_LINE_TEXT_MAX - 1] = '\0';
        s_lines[s_active_line].shown_x = s_active_x;
        s_active_line = -1;
        return; /* не начинаем новую строку в этом же вызове главного цикла */
    }

    for (uint8_t i = 0; i < TEXTFIELD_MAX_LINES; i++) {
        if (s_lines[i].used && s_lines[i].dirty) {
            /* Снапшот СЕЙЧАС, до вызова Gfx_DrawTextStart() — дальше и до
             * самого завершения задания s_lines[i].text/x могут свободно
             * меняться (TextField_Printf() и т.п.), это не повлияет на уже
             * стартовавший рендер. */
            strncpy(s_active_text, s_lines[i].text, TEXTFIELD_LINE_TEXT_MAX - 1);
            s_active_text[TEXTFIELD_LINE_TEXT_MAX - 1] = '\0';
            s_active_x = s_lines[i].x;

            Gfx_JobState_t st = Gfx_DrawTextStart(s_active_x, s_lines[i].y,
                                                   s_active_text,
                                                   s_lines[i].shown_x, s_lines[i].shown_text,
                                                   s_lines[i].font,
                                                   s_lines[i].fg, s_lines[i].bg);
            if (st == GFX_JOB_BUSY) {
                s_lines[i].dirty = false;
                s_active_line = (int)i;
            } else if (st == GFX_JOB_ERROR_RETRY) {
                /* Транзиентный отказ Display_SetWindow() при настройке окна
                 * стирания старого прямоугольника (см. gfx.c) — задание не
                 * стартовало вовсе, shown_text/shown_x (а значит и то, что
                 * реально на экране) не тронуты. Оставляем dirty=true —
                 * строка будет предпринята заново на следующей итерации
                 * главного цикла теми же (или уже новыми) данными. */
                s_lines[i].dirty = true;
            } else {
                /* GFX_JOB_ERROR_FATAL — неверные аргументы (font/text NULL),
                 * баг конфигурации строки, а не что-то, что чинится
                 * повторной попыткой. Снимаем dirty: иначе эта же строка
                 * будет вечно выбираться первой в цикле for выше и
                 * блокировать отрисовку всех остальных dirty-строк. */
                s_lines[i].dirty = false;
            }
            return; /* одна строка за вызов — не задерживаем главный цикл */
        }
    }
}
