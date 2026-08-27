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
    ITEM_RESET,
    ITEM_EXPERT,
} user_item_t;
#define USER_MENU_ITEM_COUNT (7U)

/* ---- Пункты уровня Expert ---- */
typedef enum {
    EXPERT_ITEM_EXIT = 0,
    EXPERT_ITEM_KP,
    EXPERT_ITEM_KI,
    EXPERT_ITEM_KD,
    EXPERT_ITEM_SLOPE,
    EXPERT_ITEM_BIAS,
    EXPERT_ITEM_RESET,
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
    MENU_STATE_RESET_CONFIRM,  /* промт подтверждения пункта "Сброс", ждём короткий (отмена) либо длинный (подтверждение) SET2 */
    MENU_STATE_RESET_DONE,     /* сообщение "готово" после сброса, таймер MENU_RESET_DONE_MS, см. Menu_Poll() */
} menu_internal_state_t;

/* Авто-повтор при удержании UP/DN во время редактирования числового
 * параметра — тот же многофазный алгоритм, что на главном экране (см.
 * accel_apply_step() в fsm.c): шаг 1 десять итераций -> округление до
 * ближайших 5 -> шаг 5 пять итераций -> округление до ближайших 10 -> шаг
 * 10 без ограничения по числу итераций, до отпускания кнопки. */
#define MENU_ACCEL_REPEAT_MS        (150U)
#define MENU_ACCEL_STEP1_ITERATIONS (10U)
#define MENU_ACCEL_STEP5_ITERATIONS (5U)
#define MENU_ACCEL_STEP_1  (1U)
#define MENU_ACCEL_STEP_5  (5U)
#define MENU_ACCEL_STEP_10 (10U)

typedef enum {
    MENU_ACCEL_PHASE_STEP1 = 0,
    MENU_ACCEL_PHASE_STEP5,
    MENU_ACCEL_PHASE_STEP10,
} menu_accel_phase_t;

/* Длительность показа сообщения после выполненного сброса */
#define MENU_RESET_DONE_MS (3000U)

static menu_level_t           s_level;
static uint8_t                s_cursor;
static menu_internal_state_t  s_state;

static bool               s_accel_active;
static button_id_t        s_accel_button;
static uint32_t           s_accel_last_tick;
static menu_accel_phase_t s_accel_phase;
static uint8_t            s_accel_iteration;

static uint32_t    s_reset_done_start_tick;

/* ---- Классификация текущего пункта ---- */

static bool current_item_is_toggle(void)
{
    return (s_level == MENU_LEVEL_USER) && (s_cursor == ITEM_BUZZER);
}

/**
 * @brief Прочитать текущее значение редактируемого сейчас пункта (клампинг
 *        не нужен — читаем уже закламленное значение из Settings)
 */
static uint16_t get_current_value(channel_id_t ch)
{
    if (s_level == MENU_LEVEL_USER) {
        switch (s_cursor) {
            case ITEM_PRESLEEP_TIME: return Settings_GetPreSleepTimeout(ch);
            case ITEM_PRESLEEP_TEMP: return Settings_GetPresleepTemp(ch);
            case ITEM_STANDBY:       return Settings_GetSleepTimeout(ch);
            default: return 0;
        }
    }
    switch (s_cursor) {
        case EXPERT_ITEM_KP:    return Settings_GetKp(ch);
        case EXPERT_ITEM_KI:    return Settings_GetKi(ch);
        case EXPERT_ITEM_KD:    return Settings_GetKd(ch);
        case EXPERT_ITEM_SLOPE: return Settings_GetSlope(ch);
        case EXPERT_ITEM_BIAS:  return Settings_GetBias(ch);
        default: return 0;
    }
}

/**
 * @brief Записать значение в редактируемый сейчас пункт (клампинг — уже
 *        внутри соответствующего Settings_Set*())
 */
static void set_current_value(channel_id_t ch, uint16_t value)
{
    if (s_level == MENU_LEVEL_USER) {
        switch (s_cursor) {
            case ITEM_PRESLEEP_TIME: Settings_SetPreSleepTimeout(ch, value); break;
            case ITEM_PRESLEEP_TEMP: Settings_SetPresleepTemp(ch, value);    break;
            case ITEM_STANDBY:       Settings_SetSleepTimeout(ch, value);    break;
            default: break;
        }
        return;
    }
    switch (s_cursor) {
        case EXPERT_ITEM_KP:    Settings_SetKp(ch, value);    break;
        case EXPERT_ITEM_KI:    Settings_SetKi(ch, value);    break;
        case EXPERT_ITEM_KD:    Settings_SetKd(ch, value);    break;
        case EXPERT_ITEM_SLOPE: Settings_SetSlope(ch, value); break;
        case EXPERT_ITEM_BIAS:  Settings_SetBias(ch, value);  break;
        default: break;
    }
}

/**
 * @brief Применить шаг к значению выбранного пункта (клампинг — уже внутри
 *        соответствующего Settings_Set*())
 */
static void apply_step(int32_t sign, uint16_t step)
{
    channel_id_t ch = InputFSM_GetActiveChannel();
    int32_t v = (int32_t)get_current_value(ch) + sign * (int32_t)step;
    if (v < 0) v = 0;
    set_current_value(ch, (uint16_t)v);
}

static uint16_t round_up_to_multiple(uint16_t value, uint16_t multiple)
{
    if (multiple == 0) return value;
    uint16_t remainder = (uint16_t)(value % multiple);
    if (remainder == 0) return value;
    return (uint16_t)(value - remainder + multiple);
}

static uint16_t round_down_to_multiple(uint16_t value, uint16_t multiple)
{
    if (multiple == 0) return value;
    uint16_t remainder = (uint16_t)(value % multiple);
    return (uint16_t)(value - remainder); /* remainder==0 -> value без изменений */
}

/**
 * @brief Применить один шаг авто-повтора UP/DN при редактировании,
 *        продвинуть фазу ускорения (зеркало accel_apply_step() в fsm.c)
 */
static void accel_apply_step(void)
{
    int32_t sign = (s_accel_button == BUTTON_UP) ? 1 : -1;
    uint16_t step;
    uint16_t phase_limit;
    uint16_t round_to;

    switch (s_accel_phase) {
        case MENU_ACCEL_PHASE_STEP1:
            step = MENU_ACCEL_STEP_1;
            phase_limit = MENU_ACCEL_STEP1_ITERATIONS;
            round_to = 5;
            break;
        case MENU_ACCEL_PHASE_STEP5:
            step = MENU_ACCEL_STEP_5;
            phase_limit = MENU_ACCEL_STEP5_ITERATIONS;
            round_to = 10;
            break;
        case MENU_ACCEL_PHASE_STEP10:
        default:
            step = MENU_ACCEL_STEP_10;
            phase_limit = 0; /* без ограничения — держим шаг 10 до отпускания */
            round_to = 0;
            break;
    }

    apply_step(sign, step);

    if (phase_limit != 0) {
        s_accel_iteration++;
        if (s_accel_iteration >= phase_limit) {
            channel_id_t ch = InputFSM_GetActiveChannel();
            uint16_t value_after = get_current_value(ch);
            uint16_t rounded = (sign > 0) ? round_up_to_multiple(value_after, round_to)
                                           : round_down_to_multiple(value_after, round_to);
            set_current_value(ch, rounded);

            s_accel_phase = (s_accel_phase == MENU_ACCEL_PHASE_STEP1) ? MENU_ACCEL_PHASE_STEP5
                                                                       : MENU_ACCEL_PHASE_STEP10;
            s_accel_iteration = 0;
        }
    }
}

static void accel_start(button_id_t btn)
{
    s_accel_active    = true;
    s_accel_button    = btn;
    s_accel_phase     = MENU_ACCEL_PHASE_STEP1;
    s_accel_iteration = 0;
    s_accel_last_tick = HAL_GetTick();
    accel_apply_step(); /* первый шаг сразу, не дожидаясь интервала */
}

static void toggle_buzzer(void)
{
    bool cur = Settings_GetFlagBit(SETTINGS_FLAG_BUZZER_BIT);
    Settings_SetFlagBit(SETTINGS_FLAG_BUZZER_BIT, !cur);
}

/** @brief Выполнить сброс полей текущего уровня меню для активного канала */
static void perform_reset(void)
{
    channel_id_t ch = InputFSM_GetActiveChannel();
    if (s_level == MENU_LEVEL_USER) {
        Settings_ResetUserDefaults(ch);
    } else {
        Settings_ResetExpertDefaults(ch);
    }
}

/* ---- Публичный API ---- */

void Menu_Init(void)
{
    s_level = MENU_LEVEL_USER;
    s_cursor = 0;
    s_state = MENU_STATE_LIST;
    s_accel_active = false;
    s_accel_phase = MENU_ACCEL_PHASE_STEP1;
    s_accel_iteration = 0;
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
                /* UP двигает курсор ВВЕРХ по списку (к меньшему индексу — к
                 * началу), DN — вниз (к большему индексу) — противоположно
                 * знаку sign, который заточен под смысл "UP=+1" для
                 * редактирования чисел ниже, а не под визуальную навигацию. */
                if (btn == BUTTON_UP) {
                    s_cursor = (uint8_t)((s_cursor + count - 1) % count);
                } else {
                    s_cursor = (uint8_t)((s_cursor + 1) % count);
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
                accel_start(btn);
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

        if (s_state == MENU_STATE_RESET_CONFIRM) {
            if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                s_state = MENU_STATE_LIST; /* отмена — возвращаемся к списку, курсор остаётся на "Сброс" */
            } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
                perform_reset();
                s_state = MENU_STATE_RESET_DONE;
                s_reset_done_start_tick = HAL_GetTick();
            }
            return MENU_ACTION_NONE;
        }

        if (s_state == MENU_STATE_RESET_DONE) {
            /* сообщение показывается фиксированное время (см. Menu_Poll()),
             * до истечения таймера SET2 не обрабатываем */
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
            if (s_cursor == ITEM_RESET) {
                if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                    s_state = MENU_STATE_RESET_CONFIRM;
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
                s_state = MENU_STATE_RESET_CONFIRM;
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
    if (s_state == MENU_STATE_RESET_DONE) {
        if ((HAL_GetTick() - s_reset_done_start_tick) >= MENU_RESET_DONE_MS) {
            s_state = MENU_STATE_LIST; /* остаёмся в том же уровне/на том же курсоре ("Сброс") */
        }
        return;
    }

    if (s_state != MENU_STATE_EDITING || !s_accel_active) {
        return;
    }
    if (!Buttons_IsHeld(s_accel_button)) {
        s_accel_active = false;
        return;
    }
    uint32_t now = HAL_GetTick();
    if ((now - s_accel_last_tick) >= MENU_ACCEL_REPEAT_MS) {
        accel_apply_step();
        s_accel_last_tick = now;
    }
}

const char *Menu_GetTitle(void)
{
    static char buf[48]; /* "Настройка " + "Паяльник"/"Отсос" в UTF-8 — кириллица 2 байта/символ,
                            "Настройка Паяльник" = 35 байт + '\0'; запас на будущее */
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
            case ITEM_RESET:         return "Сброс";
            case ITEM_EXPERT:        return "Expert";
            default:                 return "";
        }
    }
    switch (index) {
        case EXPERT_ITEM_EXIT:  return "Выход";
        case EXPERT_ITEM_KP:    return "Kp";
        case EXPERT_ITEM_KI:    return "Ki";
        case EXPERT_ITEM_KD:    return "Kd";
        case EXPERT_ITEM_SLOPE: return "Slope";
        case EXPERT_ITEM_BIAS:  return "Bias";
        case EXPERT_ITEM_RESET: return "Сброс";
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

bool Menu_IsShowingResetConfirm(void)
{
    return s_state == MENU_STATE_RESET_CONFIRM;
}

const char *Menu_GetResetConfirmLine(uint8_t line_index)
{
    switch (line_index) {
        case 0: return "Сбросить настройки?";
        case 1: return "SET2 (удержать) - да";
        case 2: return "SET2 (коротко) - отмена";
        default: return "";
    }
}

bool Menu_IsShowingResetDone(void)
{
    return s_state == MENU_STATE_RESET_DONE;
}

const char *Menu_GetResetDoneLine(uint8_t line_index)
{
    switch (line_index) {
        case 0: return "Настройки сброшены";
        default: return "";
    }
}
