/**
 * @file menu.c
 * @brief Реализация menu.h — см. правила в шапке заголовка.
 */

#include "menu.h"
#include "settings.h"
#include "fsm.h" /* InputFSM_GetActiveChannel() */
#include "stm32f4xx_hal.h" /* HAL_GetTick() — авто-повтор при редактировании */
#include <stdio.h>
#include <stddef.h>

/* ---- Пункты уровня User ---- */
typedef enum {
    ITEM_EXIT = 0,
    ITEM_BUZZER,
    ITEM_PRESLEEP_TIME,
    ITEM_PRESLEEP_TEMP,
    ITEM_STANDBY,
    ITEM_EXPERT,
} user_item_t;
#define USER_MENU_ITEM_COUNT (6U)

/* ---- Пункты уровня Expert ---- */
typedef enum {
    EXPERT_ITEM_EXIT = 0,
    EXPERT_ITEM_RESET,
    EXPERT_ITEM_KP,
    EXPERT_ITEM_KI,
    EXPERT_ITEM_KD,
    EXPERT_ITEM_SLOPE,
    EXPERT_ITEM_BIAS,
} expert_item_t;
#define EXPERT_MENU_ITEM_COUNT (7U)

typedef enum {
    MENU_LEVEL_USER = 0,
    MENU_LEVEL_EXPERT,
} menu_level_t;

typedef enum {
    MENU_STATE_LIST = 0,       /* обычная навигация UP/DN по пунктам */
    MENU_STATE_EDITING,        /* UP/DN меняют значение выбранного пункта */
    MENU_STATE_EXPERT_WARNING, /* предупреждение перед входом в Expert, ждём второй длинный SET2 */
} menu_internal_state_t;

/* Упрощённый (не многофазный, в отличие от главного экрана) авто-повтор
 * при удержании UP/DN во время редактирования числового параметра */
#define MENU_ACCEL_REPEAT_MS (150U)
#define MENU_ACCEL_STEP      (10U)

static menu_level_t           s_level;
static uint8_t                s_cursor;
static menu_internal_state_t  s_state;

static bool        s_accel_active;
static button_id_t s_accel_button;
static uint32_t    s_accel_last_tick;

/* ---- Классификация текущего пункта ---- */

static bool current_item_is_toggle(void)
{
    return (s_level == MENU_LEVEL_USER) && (s_cursor == ITEM_BUZZER);
}

static bool current_item_is_numeric(void)
{
    if (s_level == MENU_LEVEL_USER) {
        return s_cursor == ITEM_PRESLEEP_TIME || s_cursor == ITEM_PRESLEEP_TEMP || s_cursor == ITEM_STANDBY;
    }
    return s_cursor == EXPERT_ITEM_KP || s_cursor == EXPERT_ITEM_KI || s_cursor == EXPERT_ITEM_KD ||
           s_cursor == EXPERT_ITEM_SLOPE || s_cursor == EXPERT_ITEM_BIAS;
}

/**
 * @brief Применить шаг к значению выбранного пункта (клампинг — уже внутри
 *        соответствующего Settings_Set*())
 */
static void apply_step(int32_t sign, uint16_t step)
{
    channel_id_t ch = InputFSM_GetActiveChannel();
    int32_t delta = sign * (int32_t)step;

    if (s_level == MENU_LEVEL_USER) {
        switch (s_cursor) {
            case ITEM_PRESLEEP_TIME: {
                int32_t v = (int32_t)Settings_GetPreSleepTimeout(ch) + delta;
                if (v < 0) v = 0;
                Settings_SetPreSleepTimeout(ch, (uint16_t)v);
                break;
            }
            case ITEM_PRESLEEP_TEMP: {
                int32_t v = (int32_t)Settings_GetPresleepTemp(ch) + delta;
                if (v < 0) v = 0;
                Settings_SetPresleepTemp(ch, (uint16_t)v);
                break;
            }
            case ITEM_STANDBY: {
                int32_t v = (int32_t)Settings_GetSleepTimeout(ch) + delta;
                if (v < 0) v = 0;
                Settings_SetSleepTimeout(ch, (uint16_t)v);
                break;
            }
            default: break;
        }
        return;
    }

    switch (s_cursor) {
        case EXPERT_ITEM_KP: {
            int32_t v = (int32_t)Settings_GetKp(ch) + delta;
            if (v < 0) v = 0;
            Settings_SetKp(ch, (uint16_t)v);
            break;
        }
        case EXPERT_ITEM_KI: {
            int32_t v = (int32_t)Settings_GetKi(ch) + delta;
            if (v < 0) v = 0;
            Settings_SetKi(ch, (uint16_t)v);
            break;
        }
        case EXPERT_ITEM_KD: {
            int32_t v = (int32_t)Settings_GetKd(ch) + delta;
            if (v < 0) v = 0;
            Settings_SetKd(ch, (uint16_t)v);
            break;
        }
        case EXPERT_ITEM_SLOPE: {
            int32_t v = (int32_t)Settings_GetSlope(ch) + delta;
            if (v < 0) v = 0;
            Settings_SetSlope(ch, (uint16_t)v);
            break;
        }
        case EXPERT_ITEM_BIAS: {
            int32_t v = (int32_t)Settings_GetBias(ch) + delta;
            if (v < 0) v = 0;
            Settings_SetBias(ch, (uint16_t)v);
            break;
        }
        default: break;
    }
}

static void toggle_buzzer(void)
{
    bool cur = Settings_GetFlagBit(SETTINGS_FLAG_BUZZER_BIT);
    Settings_SetFlagBit(SETTINGS_FLAG_BUZZER_BIT, !cur);
}

/* ---- Публичный API ---- */

void Menu_Init(void)
{
    s_level = MENU_LEVEL_USER;
    s_cursor = 0;
    s_state = MENU_STATE_LIST;
    s_accel_active = false;
}

menu_action_t Menu_HandleEvent(const button_event_t *ev)
{
    /* --- UP/DN одиночные --- */
    if (ev->mask == BUTTON_MASK(BUTTON_UP) || ev->mask == BUTTON_MASK(BUTTON_DN)) {
        button_id_t btn = (ev->mask == BUTTON_MASK(BUTTON_UP)) ? BUTTON_UP : BUTTON_DN;
        int32_t sign = (btn == BUTTON_UP) ? 1 : -1;

        if (s_state == MENU_STATE_LIST) {
            if (ev->type == BUTTON_EVENT_SHORT_PRESS || ev->type == BUTTON_EVENT_LONG_PRESS) {
                uint8_t count = (s_level == MENU_LEVEL_USER) ? USER_MENU_ITEM_COUNT : EXPERT_MENU_ITEM_COUNT;
                if (sign > 0) {
                    s_cursor = (uint8_t)((s_cursor + 1) % count);
                } else {
                    s_cursor = (uint8_t)((s_cursor + count - 1) % count);
                }
            }
            return MENU_ACTION_NONE;
        }

        if (s_state == MENU_STATE_EDITING) {
            if (current_item_is_toggle()) {
                if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                    toggle_buzzer();
                }
                return MENU_ACTION_NONE;
            }
            if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                apply_step(sign, 1);
            } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
                s_accel_active = true;
                s_accel_button = btn;
                s_accel_last_tick = HAL_GetTick();
                apply_step(sign, MENU_ACCEL_STEP); /* первый шаг сразу, не дожидаясь интервала */
            }
            return MENU_ACTION_NONE;
        }

        /* MENU_STATE_EXPERT_WARNING — UP/DN игнорируем, это модальное предупреждение */
        return MENU_ACTION_NONE;
    }

    /* --- SET2 --- */
    if (ev->mask == BUTTON_MASK(BUTTON_SET2)) {
        if (s_state == MENU_STATE_EXPERT_WARNING) {
            if (ev->type == BUTTON_EVENT_LONG_PRESS) {
                s_level = MENU_LEVEL_EXPERT;
                s_cursor = 0;
                s_state = MENU_STATE_LIST;
            }
            /* короткое SET2 на предупреждении — не подтверждает, игнор */
            return MENU_ACTION_NONE;
        }

        if (s_state == MENU_STATE_EDITING) {
            s_state = MENU_STATE_LIST;
            s_accel_active = false;
            return MENU_ACTION_NONE;
        }

        /* MENU_STATE_LIST */
        if (s_level == MENU_LEVEL_USER) {
            if (s_cursor == ITEM_EXIT) {
                if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                    return MENU_ACTION_EXIT_TO_MAIN;
                }
                return MENU_ACTION_NONE;
            }
            if (s_cursor == ITEM_EXPERT) {
                if (ev->type == BUTTON_EVENT_LONG_PRESS) {
                    s_state = MENU_STATE_EXPERT_WARNING; /* короткое — игнор, только длинное показывает предупреждение */
                }
                return MENU_ACTION_NONE;
            }
            /* Bzzz/PreslipTime/PreslipTemp/Standby — редактируемые */
            s_state = MENU_STATE_EDITING;
            return MENU_ACTION_NONE;
        }

        /* MENU_LEVEL_EXPERT */
        if (s_cursor == EXPERT_ITEM_EXIT) {
            if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                s_level = MENU_LEVEL_USER;
                s_cursor = 0;
            }
            return MENU_ACTION_NONE;
        }
        if (s_cursor == EXPERT_ITEM_RESET) {
            if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                Settings_ResetToDefaults();
            }
            return MENU_ACTION_NONE;
        }
        /* Kp/Ki/Kd/Slope/Bias — редактируемые */
        s_state = MENU_STATE_EDITING;
        return MENU_ACTION_NONE;
    }

    /* SET1/SET3 (длинные — короткие перехвачены в fsm.c как глобальный выход)
     * и BUTTON_EVENT_CHORD_SHORT — в меню не определены, игнорируем молча. */
    return MENU_ACTION_NONE;
}

void Menu_Poll(void)
{
    if (s_state != MENU_STATE_EDITING || !s_accel_active) {
        return;
    }
    if (!Buttons_IsHeld(s_accel_button)) {
        s_accel_active = false;
        return;
    }
    uint32_t now = HAL_GetTick();
    if ((now - s_accel_last_tick) >= MENU_ACCEL_REPEAT_MS) {
        int32_t sign = (s_accel_button == BUTTON_UP) ? 1 : -1;
        apply_step(sign, MENU_ACCEL_STEP);
        s_accel_last_tick = now;
    }
}

const char *Menu_GetTitle(void)
{
    static char buf[24];
    channel_id_t ch = InputFSM_GetActiveChannel();
    snprintf(buf, sizeof(buf), "Настройка %s", (ch == CHANNEL_SOLDER) ? "Паяльник" : "Отсос");
    return buf;
}

uint8_t Menu_GetItemCount(void)
{
    return (s_level == MENU_LEVEL_USER) ? USER_MENU_ITEM_COUNT : EXPERT_MENU_ITEM_COUNT;
}

const char *Menu_GetItemLabel(uint8_t index)
{
    if (s_level == MENU_LEVEL_USER) {
        switch (index) {
            case ITEM_EXIT:          return "Выход";
            case ITEM_BUZZER:        return "Bzzz";
            case ITEM_PRESLEEP_TIME: return "PreslipTime";
            case ITEM_PRESLEEP_TEMP: return "PreslipTemp";
            case ITEM_STANDBY:       return "Standby";
            case ITEM_EXPERT:        return "Expert";
            default:                 return "";
        }
    }
    switch (index) {
        case EXPERT_ITEM_EXIT:  return "Выход";
        case EXPERT_ITEM_RESET: return "Сброс";
        case EXPERT_ITEM_KP:    return "Kp";
        case EXPERT_ITEM_KI:    return "Ki";
        case EXPERT_ITEM_KD:    return "Kd";
        case EXPERT_ITEM_SLOPE: return "Slope";
        case EXPERT_ITEM_BIAS:  return "Bias";
        default:                return "";
    }
}

void Menu_GetItemValueText(uint8_t index, char *buf, uint8_t buf_size)
{
    if (buf == NULL || buf_size == 0) return;
    buf[0] = '\0';

    channel_id_t ch = InputFSM_GetActiveChannel();

    if (s_level == MENU_LEVEL_USER) {
        switch (index) {
            case ITEM_BUZZER:
                snprintf(buf, buf_size, "%s", Settings_GetFlagBit(SETTINGS_FLAG_BUZZER_BIT) ? "ON" : "OFF");
                break;
            case ITEM_PRESLEEP_TIME:
                snprintf(buf, buf_size, "%u", (unsigned)Settings_GetPreSleepTimeout(ch));
                break;
            case ITEM_PRESLEEP_TEMP:
                snprintf(buf, buf_size, "%u", (unsigned)Settings_GetPresleepTemp(ch));
                break;
            case ITEM_STANDBY:
                snprintf(buf, buf_size, "%u", (unsigned)Settings_GetSleepTimeout(ch));
                break;
            default:
                break; /* Выход/Expert — без значения */
        }
        return;
    }

    switch (index) {
        case EXPERT_ITEM_KP:    snprintf(buf, buf_size, "%u", (unsigned)Settings_GetKp(ch));    break;
        case EXPERT_ITEM_KI:    snprintf(buf, buf_size, "%u", (unsigned)Settings_GetKi(ch));    break;
        case EXPERT_ITEM_KD:    snprintf(buf, buf_size, "%u", (unsigned)Settings_GetKd(ch));    break;
        case EXPERT_ITEM_SLOPE: snprintf(buf, buf_size, "%u", (unsigned)Settings_GetSlope(ch)); break;
        case EXPERT_ITEM_BIAS:  snprintf(buf, buf_size, "%u", (unsigned)Settings_GetBias(ch));  break;
        default: break; /* Выход/Сброс — без значения */
    }
}

uint8_t Menu_GetCursor(void)
{
    return s_cursor;
}

bool Menu_IsEditing(void)
{
    return s_state == MENU_STATE_EDITING;
}

bool Menu_IsShowingExpertWarning(void)
{
    return s_state == MENU_STATE_EXPERT_WARNING;
}

const char *Menu_GetExpertWarningLine(uint8_t line_index)
{
    switch (line_index) {
        case 0: return "Внимание!!!";
        case 1: return "Режим требует квалификации!";
        case 2: return "Неверные настройки могут повредить инструмент.";
        default: return "";
    }
}
