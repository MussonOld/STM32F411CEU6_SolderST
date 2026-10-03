/**
 * @file    screen_menu.c
 * @brief   Экран сервисного меню — см. screen_menu.h.
 */
#include "screen_menu.h"
#include "screen_layout.h"
#include "text_field.h"
#include "fonts.h"
#include "menu.h"
#include "fsm.h"       /* InputFSM_GetActiveChannel() */
#include "error.h"     /* Error_IsChannelBlocked() */
#include "ads1220.h"    /* ADS1220_IsDataValid()/GetTemperatureC() — живое измерение, см. render_live_temp() */
#include "fixed_point.h"
#include "channel.h"
#include <stdint.h>
#include <stdbool.h>

void ScreenMenu_Init(void)
{
    /* Сервисное меню — шрифт 18 везде (см. спецификацию), позиции по
     * вертикали друг под другом, x общий для title и всех строк списка */
    TextField_ConfigureLine(LINE_MENU_TITLE, SCREEN_MENU_TITLE_X, SCREEN_MENU_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_MENU_TITLE, COLOR_BG);
    for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
        TextField_ConfigureLine((uint8_t)(LINE_MENU_ITEM_0 + i), SCREEN_MENU_ITEM_X,
                                 (uint16_t)(SCREEN_MENU_ITEM_Y0 + i * SCREEN_MENU_ITEM_STEP),
                                 &AntiquaB_18_uni, COLOR_MENU_NORMAL, COLOR_BG);
    }
    /* Живая температура на пунктах калибровки — крупно в правой половине экрана (x — центр) */
    TextField_ConfigureLine(LINE_MENU_TEMP, SCREEN_MENU_TEMP_CENTER_X, SCREEN_MENU_TEMP_Y,
                             &Comic_60_dig, COLOR_ACTIVE_CURRENT, COLOR_BG);
}

typedef const char *(*menu_text_line_fn)(uint8_t line_index);

/** @brief Экран-сообщение меню: первые count строк — текст из getter, остальные пустые, цвет обычный. */
static void render_menu_message(menu_text_line_fn getter, uint8_t count)
{
    for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
        uint8_t line = (uint8_t)(LINE_MENU_ITEM_0 + i);
        if (i < count) {
            TextField_Printf(line, "%s", getter(i));
        } else {
            TextField_Printf(line, "");
        }
        TextField_SetColors(line, COLOR_MENU_NORMAL, COLOR_BG);
    }
}

static display_color_t menu_item_color(uint8_t i, uint8_t cursor, bool editing)
{
    if (i != cursor) {
        return COLOR_MENU_NORMAL;
    }
    return editing ? COLOR_MENU_EDITING : COLOR_MENU_CURSOR;
}

/** @brief Строка пункта: "Название  значение" либо просто "Название" (у пунктов без значения). */
static void print_menu_item(uint8_t line, uint8_t i)
{
    char value[16];
    Menu_GetItemValueText(i, value, sizeof(value));
    if (value[0] != '\0') {
        TextField_Printf(line, "%s  %s", Menu_GetItemLabel(i), value);
    } else {
        TextField_Printf(line, "%s", Menu_GetItemLabel(i));
    }
}

static void render_menu_items(void)
{
    uint8_t count = Menu_GetItemCount();
    uint8_t cursor = Menu_GetCursor();
    bool editing = Menu_IsEditing();

    for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
        uint8_t line = (uint8_t)(LINE_MENU_ITEM_0 + i);

        if (i < count) {
            print_menu_item(line, i);
        } else {
            TextField_Printf(line, ""); /* уровень User короче Expert — лишние строки пустые */
        }
        TextField_SetColors(line, menu_item_color(i, cursor, editing), COLOR_BG);
    }
}

/**
 * @brief Отрисовать экран сервисного меню целиком (заменяет главный экран)
 */
/* ---- Живая температура на пунктах калибровки (Slope/Bias) ---- */

#define SCREEN_MENU_TEMP_HYST_C FIXED_FROM_FLOAT(0.3f) /* как на главном экране: целое не дребезжит на границе */

static int32_t s_temp_shown[CHANNEL_COUNT];
static bool    s_temp_shown_valid[CHANNEL_COUNT];

/** @brief Целое для показа с гистерезисом (то же правило, что temp_for_display() на главном экране; состояние своё). */
static int32_t menu_temp_for_display(channel_id_t ch, fixed_t cur)
{
    if (s_temp_shown_valid[ch]) {
        fixed_t lo = FIXED_FROM_INT(s_temp_shown[ch]) - SCREEN_MENU_TEMP_HYST_C;
        fixed_t hi = FIXED_FROM_INT(s_temp_shown[ch] + 1) + SCREEN_MENU_TEMP_HYST_C;
        if (cur >= lo && cur < hi) {
            return s_temp_shown[ch];
        }
    }
    s_temp_shown[ch] = FIXED_TO_INT(cur);
    s_temp_shown_valid[ch] = true;
    return s_temp_shown[ch];
}

/**
 * @brief Живая температура активного канала на пунктах калибровки.
 *
 * Берётся ПРЯМО из АЦП (ADS1220_GetTemperatureC(), формула применяет Slope/Bias
 * из Settings на каждом чтении): значение State_GetCurrentTemp() обновляет
 * только Control, и то лишь пока нагрев разрешён — в SLEEP и у выключенного
 * канала оно замирает, а при калибровке показание должно быть живым всегда
 * (подставил новый Slope/Bias — через отсчёт АЦП видно новое число).
 * Не показывается (пустое поле), если пункт не калибровочный, канал
 * заблокирован (не подключен/авария/БП) или отсчёт АЦП невалиден.
 */
static void render_live_temp(void)
{
    channel_id_t ch = InputFSM_GetActiveChannel();

    if (Menu_ShowsLiveTemp() && !Error_IsChannelBlocked(ch) && ADS1220_IsDataValid(ch)) {
        TextField_PrintfCentered(LINE_MENU_TEMP, SCREEN_MENU_TEMP_CENTER_X, "%ld",
                                 (long)menu_temp_for_display(ch, ADS1220_GetTemperatureC(ch)));
    } else {
        s_temp_shown_valid[ch] = false; /* вернёмся на пункт — гистерезис начнём заново */
        TextField_PrintfCentered(LINE_MENU_TEMP, SCREEN_MENU_TEMP_CENTER_X, "");
    }
}

void ScreenMenu_Render(void)
{
    TextField_Printf(LINE_MENU_TITLE, "%s", Menu_GetTitle());

    if (Menu_IsShowingExpertWarning()) {
        render_menu_message(Menu_GetExpertWarningLine, 4);
    } else if (Menu_IsShowingResetConfirm()) {
        render_menu_message(Menu_GetResetConfirmLine, 3);
    } else if (Menu_IsShowingResetDone()) {
        render_menu_message(Menu_GetResetDoneLine, 3);
    } else {
        render_menu_items();
    }

    render_live_temp();
}
