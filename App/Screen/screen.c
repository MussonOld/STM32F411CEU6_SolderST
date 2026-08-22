/**
 * @file screen.c
 * @brief Реализация screen.h — см. правила в шапке заголовка.
 *
 * Разметка 320x240 (см. обсуждение):
 *  - y=0..29   : общая инфозона (сейчас пустая — содержимое не определено,
 *                таймеры сна появятся здесь позже)
 *  - x=159..160: вертикальный разделитель на всю высоту ниже инфозоны
 *  - каждая половина (0..158 / 161..319):
 *      - текущая температура, шрифт Comic_40_dig (крупный, только цифры)
 *      - целевая температура, шрифт AntiquaB_18_uni (ВРЕМЕННО — по ТЗ
 *        будет убрана позже, сейчас нужна для отладки)
 *      - строка пресетов, шрифт AntiquaB_24_uni, внизу
 *
 * Координаты — первый приближённый вариант (не откалиброван визуально на
 * реальном дисплее из этой сессии) — при необходимости подвинуть на
 * реальном железе, ничего в остальной архитектуре это не затронет.
 */

#include "screen.h"
#include "text_field.h"
#include "display.h"
#include "fonts.h"
#include "channel.h"
#include "state.h"
#include "settings.h"
#include "fsm.h"
#include "fixed_point.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Индексы строк TextField ---- */
enum {
    LINE_INFO = 0,
    LINE_SOLDER_CURRENT,
    LINE_SOLDER_TARGET,
    LINE_SOLDER_PRESETS,
    LINE_DESOLDER_CURRENT,
    LINE_DESOLDER_TARGET,
    LINE_DESOLDER_PRESETS,
};

/* ---- Геометрия ---- */
#define SCREEN_WIDTH        (320U)
#define SCREEN_HEIGHT       (240U)
#define SCREEN_INFO_HEIGHT  (30U)
#define SCREEN_DIVIDER_X0   (159U)
#define SCREEN_DIVIDER_X1   (160U) /* разделитель 2px шириной: X0..X1 включительно */

#define SCREEN_LEFT_X    (30U)
#define SCREEN_RIGHT_X   (190U)
#define SCREEN_CURRENT_Y (70U)
#define SCREEN_TARGET_Y  (130U)
#define SCREEN_PRESETS_Y (210U)
#define SCREEN_PRESETS_LEFT_X   (10U)
#define SCREEN_PRESETS_RIGHT_X  (170U)
#define SCREEN_INFO_X (10U)
#define SCREEN_INFO_Y (6U)

/* ---- Цвета ---- */
#define COLOR_BG               DISPLAY_RGB565(0, 0, 0)
#define COLOR_ACTIVE_CURRENT   DISPLAY_RGB565(255, 255, 255)
#define COLOR_INACTIVE_CURRENT DISPLAY_RGB565(90, 90, 90)
#define COLOR_ACTIVE_TARGET    DISPLAY_RGB565(180, 180, 180)
#define COLOR_INACTIVE_TARGET  DISPLAY_RGB565(60, 60, 60)
#define COLOR_ACTIVE_PRESETS   DISPLAY_RGB565(255, 210, 0)
#define COLOR_INACTIVE_PRESETS DISPLAY_RGB565(90, 75, 0)
#define COLOR_DIVIDER          DISPLAY_RGB565(100, 100, 100)
#define COLOR_INFO             DISPLAY_RGB565(255, 255, 255)

static channel_id_t s_last_active_channel;

/**
 * @brief Применить цвета трёх строк канала (current/target/presets) под
 *        активное или неактивное состояние
 */
static void apply_channel_colors(channel_id_t ch, bool active)
{
    uint8_t line_current, line_target, line_presets;

    if (ch == CHANNEL_SOLDER) {
        line_current = LINE_SOLDER_CURRENT;
        line_target  = LINE_SOLDER_TARGET;
        line_presets = LINE_SOLDER_PRESETS;
    } else {
        line_current = LINE_DESOLDER_CURRENT;
        line_target  = LINE_DESOLDER_TARGET;
        line_presets = LINE_DESOLDER_PRESETS;
    }

    TextField_SetColors(line_current, active ? COLOR_ACTIVE_CURRENT : COLOR_INACTIVE_CURRENT, COLOR_BG);
    TextField_SetColors(line_target,  active ? COLOR_ACTIVE_TARGET  : COLOR_INACTIVE_TARGET,  COLOR_BG);
    TextField_SetColors(line_presets, active ? COLOR_ACTIVE_PRESETS : COLOR_INACTIVE_PRESETS, COLOR_BG);
}

/**
 * @brief Нарисовать статический вертикальный разделитель (однократно, блокирующе)
 */
static void draw_divider(void)
{
    uint16_t width = (uint16_t)(SCREEN_DIVIDER_X1 - SCREEN_DIVIDER_X0 + 1);
    uint16_t height = (uint16_t)(SCREEN_HEIGHT - SCREEN_INFO_HEIGHT);

    if (Display_SetWindow(SCREEN_DIVIDER_X0, SCREEN_INFO_HEIGHT,
                           SCREEN_DIVIDER_X1, SCREEN_HEIGHT - 1) == DISPLAY_OK) {
        Display_FillColorDMA(COLOR_DIVIDER, (uint32_t)width * height);
        while (Display_IsBusy()) { } /* однократно, при старте, до входа в главный цикл */
    }
}

void Screen_Init(void)
{
    draw_divider();

    TextField_ConfigureLine(LINE_INFO, SCREEN_INFO_X, SCREEN_INFO_Y,
                             &AntiquaB_16_uni, COLOR_INFO, COLOR_BG);
    /* Содержимое инфозоны не определено — оставляем пустой, TODO позже
     * (таймеры сна и т.п.), геометрия уже готова. */

    TextField_ConfigureLine(LINE_SOLDER_CURRENT, SCREEN_LEFT_X, SCREEN_CURRENT_Y,
                             &Comic_40_dig, COLOR_ACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_TARGET, SCREEN_LEFT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_ACTIVE_TARGET, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_PRESETS, SCREEN_PRESETS_LEFT_X, SCREEN_PRESETS_Y,
                             &AntiquaB_24_uni, COLOR_ACTIVE_PRESETS, COLOR_BG);

    TextField_ConfigureLine(LINE_DESOLDER_CURRENT, SCREEN_RIGHT_X, SCREEN_CURRENT_Y,
                             &Comic_40_dig, COLOR_INACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_TARGET, SCREEN_RIGHT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_INACTIVE_TARGET, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_PRESETS, SCREEN_PRESETS_RIGHT_X, SCREEN_PRESETS_Y,
                             &AntiquaB_24_uni, COLOR_INACTIVE_PRESETS, COLOR_BG);

    /* Solder активен по умолчанию при старте (см. InputFSM_Init()) —
     * начальные цвета выше уже расставлены соответственно. */
    s_last_active_channel = CHANNEL_SOLDER;
}

/**
 * @brief Обновить содержимое трёх строк одного канала из State/Settings
 */
static void update_channel_content(channel_id_t ch, uint8_t line_current,
                                    uint8_t line_target, uint8_t line_presets)
{
    fixed_t cur = State_GetCurrentTemp(ch);
    int32_t cur_int = FIXED_TO_INT(cur);
    TextField_Printf(line_current, "%ld", (long)cur_int);

    uint16_t target = Settings_GetTarget(ch);
    TextField_Printf(line_target, "%u", (unsigned)target);

    uint16_t p1 = Settings_GetPreset(ch, PRESET_1);
    uint16_t p2 = Settings_GetPreset(ch, PRESET_2);
    uint16_t p3 = Settings_GetPreset(ch, PRESET_3);
    TextField_Printf(line_presets, "%u %u %u", (unsigned)p1, (unsigned)p2, (unsigned)p3);
}

void Screen_Update(void)
{
    update_channel_content(CHANNEL_SOLDER, LINE_SOLDER_CURRENT, LINE_SOLDER_TARGET, LINE_SOLDER_PRESETS);
    update_channel_content(CHANNEL_DESOLDER, LINE_DESOLDER_CURRENT, LINE_DESOLDER_TARGET, LINE_DESOLDER_PRESETS);

    channel_id_t active = InputFSM_GetActiveChannel();
    if (active != s_last_active_channel) {
        apply_channel_colors(s_last_active_channel, false);
        apply_channel_colors(active, true);
        s_last_active_channel = active;
    }
}
