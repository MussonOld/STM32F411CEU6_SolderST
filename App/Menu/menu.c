/**
 * @file menu.c
 * @brief Реализация menu.h — см. правила в шапке заголовка.
 *
 * Пункты обоих уровней описаны таблицами s_user_items/s_expert_items (подпись,
 * роль, геттер/сеттер Settings); Menu_HandleEvent() только разбирает кнопку и
 * состояние и делегирует по роли пункта под курсором.
 */

#include "menu.h"
#include "settings.h"
#include "fsm.h" /* InputFSM_GetActiveChannel() */
#include "step_accel.h" /* авто-повтор UP/DN — общий с fsm.c, см. App/Common */
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
    ITEM_SPLASH,
    ITEM_RESET,
    ITEM_EXPERT,
} user_item_t;
#define USER_MENU_ITEM_COUNT (8U)

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
 * параметра — тот же многофазный алгоритм, что на главном экране; общая
 * реализация в Common/step_accel.h. */

/* Длительность показа сообщения после выполненного сброса */
#define MENU_RESET_DONE_MS (3000U)

static menu_level_t           s_level;
static uint8_t                s_cursor;
static menu_internal_state_t  s_state;

static step_accel_t s_accel;

static uint32_t    s_reset_done_start_tick;

/* ---- Таблицы пунктов ----
 *
 * Каждый пункт — одна строка таблицы: подпись, роль (что делают SET2 и UP/DN)
 * и, у пунктов со значением, геттер/сеттер Settings для активного канала.
 * Подписи, тексты значений, редактирование и реакция на SET2 берутся отсюда —
 * поэтому добавление пункта (как было с "Заставкой") сводится к строке в
 * таблице и одному значению enum выше. */
typedef enum {
    ROLE_EXIT,   /* "Выход": SET2 (коротко) — выйти из меню (User) / вернуться на уровень User (Expert) */
    ROLE_TOGGLE, /* "Bzzz": ON/OFF, короткое UP/DN переключает */
    ROLE_CHOICE, /* "Заставка": число 0..2, только короткое UP/DN ±1 с упором в границы, без авто-повтора */
    ROLE_NUMBER, /* числовой параметр канала: короткое UP/DN ±1, удержание — авто-повтор (step_accel) */
    ROLE_RESET,  /* "Сброс": SET2 (коротко) — запросить подтверждение */
    ROLE_EXPERT, /* "Expert": SET2 (длинное) — предупреждение, затем вход в Expert */
} item_role_t;

typedef struct {
    const char *label;
    item_role_t role;
    uint16_t  (*get)(channel_id_t ch); /* NULL — у пункта нет числового значения (Выход/Сброс/Expert/Bzzz) */
    void      (*set)(channel_id_t ch, uint16_t value); /* только ROLE_NUMBER; клампинг — внутри Settings_Set*() */
} item_desc_t;

/** @brief "Заставка" — глобальная (не по каналам), поэтому канал игнорируется */
static uint16_t get_splash_mode(channel_id_t ch)
{
    (void)ch;
    return Settings_GetSplashMode();
}

static const item_desc_t s_user_items[USER_MENU_ITEM_COUNT] = {
    [ITEM_EXIT]          = { "Выход",        ROLE_EXIT,   NULL,                      NULL },
    [ITEM_BUZZER]        = { "Bzzz",         ROLE_TOGGLE, NULL,                      NULL },
    [ITEM_PRESLEEP_TIME] = { "PresleepTime", ROLE_NUMBER, Settings_GetPreSleepTimeout, Settings_SetPreSleepTimeout },
    [ITEM_PRESLEEP_TEMP] = { "PresleepTemp", ROLE_NUMBER, Settings_GetPresleepTemp,    Settings_SetPresleepTemp },
    [ITEM_STANDBY]       = { "Standby",      ROLE_NUMBER, Settings_GetSleepTimeout,    Settings_SetSleepTimeout },
    [ITEM_SPLASH]        = { "Заставка",     ROLE_CHOICE, get_splash_mode,             NULL },
    [ITEM_RESET]         = { "Сброс",        ROLE_RESET,  NULL,                        NULL },
    [ITEM_EXPERT]        = { "Expert",       ROLE_EXPERT, NULL,                        NULL },
};

static const item_desc_t s_expert_items[EXPERT_MENU_ITEM_COUNT] = {
    [EXPERT_ITEM_EXIT]  = { "Выход", ROLE_EXIT,   NULL,               NULL },
    [EXPERT_ITEM_KP]    = { "Kp",    ROLE_NUMBER, Settings_GetKp,    Settings_SetKp },
    [EXPERT_ITEM_KI]    = { "Ki",    ROLE_NUMBER, Settings_GetKi,    Settings_SetKi },
    [EXPERT_ITEM_KD]    = { "Kd",    ROLE_NUMBER, Settings_GetKd,    Settings_SetKd },
    [EXPERT_ITEM_SLOPE] = { "Slope", ROLE_NUMBER, Settings_GetSlope, Settings_SetSlope },
    [EXPERT_ITEM_BIAS]  = { "Bias",  ROLE_NUMBER, Settings_GetBias,  Settings_SetBias },
    [EXPERT_ITEM_RESET] = { "Сброс", ROLE_RESET,  NULL,               NULL },
};

/** Индекс вне списка уровня: пустая подпись, значения нет, пункт не числовой — ведёт себя "как нет такого пункта". */
static const item_desc_t s_no_item = { "", ROLE_EXIT, NULL, NULL };

static const item_desc_t *item_at(menu_level_t level, uint8_t index)
{
    if (level == MENU_LEVEL_USER) {
        return (index < USER_MENU_ITEM_COUNT) ? &s_user_items[index] : &s_no_item;
    }
    return (index < EXPERT_MENU_ITEM_COUNT) ? &s_expert_items[index] : &s_no_item;
}

static const item_desc_t *current_item(void)
{
    return item_at(s_level, s_cursor);
}

/**
 * @brief Прочитать текущее значение редактируемого сейчас пункта (клампинг
 *        не нужен — читаем уже закламленное значение из Settings)
 */
static uint16_t get_current_value(channel_id_t ch)
{
    const item_desc_t *it = current_item();
    return (it->role == ROLE_NUMBER && it->get != NULL) ? it->get(ch) : 0;
}

/**
 * @brief Записать значение в редактируемый сейчас пункт (клампинг — уже
 *        внутри соответствующего Settings_Set*())
 */
static void set_current_value(channel_id_t ch, uint16_t value)
{
    const item_desc_t *it = current_item();
    if (it->role == ROLE_NUMBER && it->set != NULL) {
        it->set(ch, value);
    }
}

/* Колбэки для step_accel: читают/пишут текущий редактируемый пункт через
 * уже существующие get_current_value()/set_current_value() (там же клампинг). */
static uint16_t accel_get(void *ctx)
{
    (void)ctx;
    return get_current_value(InputFSM_GetActiveChannel());
}

static void accel_set(void *ctx, uint16_t value)
{
    (void)ctx;
    set_current_value(InputFSM_GetActiveChannel(), value);
}

static void accel_start(button_id_t btn)
{
    StepAccel_Start(&s_accel, btn, accel_get, accel_set, NULL, HAL_GetTick());
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
        Settings_ResetGlobalUserDefaults(); /* Bzzz — общий на оба канала */
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
    StepAccel_Stop(&s_accel);
}

/* ---- UP/DN одиночные ---- */

static bool is_press(const button_event_t *ev)
{
    return ev->type == BUTTON_EVENT_SHORT_PRESS || ev->type == BUTTON_EVENT_LONG_PRESS;
}

/**
 * @brief Сдвинуть курсор по кругу. UP двигает курсор ВВЕРХ по списку (к
 *        меньшему индексу — к началу), DN — вниз (к большему индексу) —
 *        противоположно знаку sign из редактирования чисел (UP=+1), это
 *        визуальная навигация, а не изменение значения.
 */
static void move_cursor(button_id_t btn)
{
    uint8_t count = Menu_GetItemCount();
    if (btn == BUTTON_UP) {
        s_cursor = (uint8_t)((s_cursor + count - 1) % count);
    } else {
        s_cursor = (uint8_t)((s_cursor + 1) % count);
    }
}

/** @brief "Заставка": шаг ±1 с упором в границы (верхнюю клампит Settings_SetSplashMode()) */
static void step_choice(int32_t sign)
{
    int32_t v = (int32_t)Settings_GetSplashMode() + sign;
    if (v < 0) v = 0;
    Settings_SetSplashMode((uint8_t)v);
}

/** @brief UP/DN в режиме редактирования — по роли текущего пункта */
static void edit_current_item(const button_event_t *ev, button_id_t btn)
{
    int32_t sign = (btn == BUTTON_UP) ? 1 : -1;
    bool is_short = (ev->type == BUTTON_EVENT_SHORT_PRESS);

    switch (current_item()->role) {
        case ROLE_TOGGLE:
            if (is_short) {
                toggle_buzzer();
            }
            break;
        case ROLE_CHOICE:
            if (is_short) {
                step_choice(sign);
            }
            break;
        default: /* числовой: короткое — шаг 1, длинное — авто-повтор до отпускания (Menu_Poll) */
            if (is_short) {
                StepAccel_ApplyDelta(accel_get, accel_set, NULL, sign, 1);
            } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
                accel_start(btn);
            }
            break;
    }
}

static void handle_up_dn(const button_event_t *ev)
{
    button_id_t btn = (ev->mask == BUTTON_MASK(BUTTON_UP)) ? BUTTON_UP : BUTTON_DN;

    switch (s_state) {
        case MENU_STATE_LIST:
            if (is_press(ev)) {
                move_cursor(btn);
            }
            break;
        case MENU_STATE_EDITING:
            edit_current_item(ev, btn);
            break;
        default:
            break; /* EXPERT_WARNING/RESET_CONFIRM/RESET_DONE — модальные, UP/DN игнорируем */
    }
}

/* ---- SET2 ---- */

static void set2_on_expert_warning(const button_event_t *ev)
{
    if (ev->type == BUTTON_EVENT_LONG_PRESS) {
        s_level = MENU_LEVEL_EXPERT;
        s_cursor = 0;
        s_state = MENU_STATE_LIST;
    }
    /* короткое SET2 на предупреждении — не подтверждает, игнор */
}

static void set2_on_reset_confirm(const button_event_t *ev)
{
    if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
        s_state = MENU_STATE_LIST; /* отмена — возвращаемся к списку, курсор остаётся на "Сброс" */
    } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
        perform_reset();
        s_state = MENU_STATE_RESET_DONE;
        s_reset_done_start_tick = HAL_GetTick();
    }
}

static menu_action_t set2_on_exit_item(const button_event_t *ev)
{
    if (ev->type != BUTTON_EVENT_SHORT_PRESS) {
        return MENU_ACTION_NONE;
    }
    if (s_level == MENU_LEVEL_USER) {
        return MENU_ACTION_EXIT_TO_MAIN;
    }
    s_level = MENU_LEVEL_USER; /* "Выход" уровня Expert — назад на уровень User */
    s_cursor = 0;
    return MENU_ACTION_NONE;
}

/** @brief SET2 в списке — по роли пункта под курсором */
static menu_action_t set2_in_list(const button_event_t *ev)
{
    switch (current_item()->role) {
        case ROLE_EXIT:
            return set2_on_exit_item(ev);
        case ROLE_RESET:
            if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
                s_state = MENU_STATE_RESET_CONFIRM;
            }
            break;
        case ROLE_EXPERT:
            if (ev->type == BUTTON_EVENT_LONG_PRESS) {
                s_state = MENU_STATE_EXPERT_WARNING; /* короткое — игнор, только длинное показывает предупреждение */
            }
            break;
        default: /* Bzzz/PresleepTime/PresleepTemp/Standby/Заставка и Kp/Ki/Kd/Slope/Bias — редактируемые */
            s_state = MENU_STATE_EDITING;
            break;
    }
    return MENU_ACTION_NONE;
}

static menu_action_t handle_set2(const button_event_t *ev)
{
    switch (s_state) {
        case MENU_STATE_EXPERT_WARNING:
            set2_on_expert_warning(ev);
            break;
        case MENU_STATE_RESET_CONFIRM:
            set2_on_reset_confirm(ev);
            break;
        case MENU_STATE_RESET_DONE:
            /* сообщение показывается фиксированное время (см. Menu_Poll()),
             * до истечения таймера SET2 не обрабатываем */
            break;
        case MENU_STATE_EDITING:
            s_state = MENU_STATE_LIST;
            s_accel.active = false;
            break;
        case MENU_STATE_LIST:
        default:
            return set2_in_list(ev);
    }
    return MENU_ACTION_NONE;
}

menu_action_t Menu_HandleEvent(const button_event_t *ev)
{
    if (ev->mask == BUTTON_MASK(BUTTON_UP) || ev->mask == BUTTON_MASK(BUTTON_DN)) {
        handle_up_dn(ev);
        return MENU_ACTION_NONE;
    }
    if (ev->mask == BUTTON_MASK(BUTTON_SET2)) {
        return handle_set2(ev);
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

    if (s_state != MENU_STATE_EDITING || !s_accel.active) {
        return;
    }
    if (!Buttons_IsHeld(s_accel.button)) {
        s_accel.active = false;
        return;
    }
    StepAccel_Tick(&s_accel, HAL_GetTick());
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
    return item_at(s_level, index)->label;
}

void Menu_GetItemValueText(uint8_t index, char *buf, uint8_t buf_size)
{
    if (buf == NULL || buf_size == 0) return;
    buf[0] = '\0';

    const item_desc_t *it = item_at(s_level, index);

    if (it->role == ROLE_TOGGLE) {
        snprintf(buf, buf_size, "%s", Settings_GetFlagBit(SETTINGS_FLAG_BUZZER_BIT) ? "ON" : "OFF");
    } else if (it->get != NULL) {
        snprintf(buf, buf_size, "%u", (unsigned)it->get(InputFSM_GetActiveChannel()));
    } /* иначе (Выход/Сброс/Expert) — без значения */
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
        case 2: return "Неверные настройки могут";
        case 3: return "повредить инструмент.";
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
