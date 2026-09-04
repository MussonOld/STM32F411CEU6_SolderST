/**
 * @file fsm.c
 * @brief Реализация fsm.h — см. правила в шапке заголовка.
 */

#include <stdint.h>
#include <stdbool.h>
#include "fsm.h"
#include "buttons.h"
#include "settings.h"
#include "state.h"
#include "error.h"
#include "menu.h"
#include "sleep.h"
#include "fixed_point.h"
#include "step_accel.h" /* авто-повтор UP/DN — общий с menu.c, см. App/Common */
#include "stm32f4xx_hal.h" /* HAL_GetTick() — интервал авто-повтора UP/DN */

static channel_id_t s_active_channel;
static screen_mode_t s_screen_mode;

static step_accel_t s_accel;

/* ---- Внутренние примитивы ---- */

static preset_id_t preset_for_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_SET1: return PRESET_1;
        case BUTTON_SET2: return PRESET_2;
        case BUTTON_SET3: return PRESET_3;
        default:          return PRESET_1; /* не должно вызываться для других кнопок */
    }
}

/**
 * @brief Записать target в Settings (клампится там) и сразу продублировать
 *        в State для PID — единственная точка входа для этой пары записей.
 */
static void apply_target_and_sync(channel_id_t ch, int32_t requested_target)
{
    if (requested_target < 0) requested_target = 0;
    Settings_SetTarget(ch, (uint16_t)requested_target);
    uint16_t clamped = Settings_GetTarget(ch); /* реальное значение после клампа диапазона 50..450 */
    State_SetSetpointTemp(ch, FIXED_FROM_INT(clamped));
}

/* Колбэки для step_accel: читают/пишут target активного канала через
 * apply_target_and_sync() (там же клампинг диапазона и синхронизация в State). */
static uint16_t accel_get(void *ctx)
{
    (void)ctx;
    return Settings_GetTarget(s_active_channel);
}

static void accel_set(void *ctx, uint16_t value)
{
    (void)ctx;
    apply_target_and_sync(s_active_channel, (int32_t)value);
}

static void accel_start(button_id_t btn)
{
    StepAccel_Start(&s_accel, btn, accel_get, accel_set, NULL, HAL_GetTick());
}

/**
 * @brief Разобрать одно событие от Buttons и применить действие
 */
static void dispatch_event(const button_event_t *ev)
{
    /* --- TOOLS: переключение активного канала — работает ВСЕГДА, в любом
     * режиме экрана (в т.ч. внутри сервисного меню — оно наследует активный
     * канал и TOOLS может переключить его прямо оттуда, см. menu.h).
     * Не передаём фокус на неисправный канал (Error_IsChannelBlocked) — если
     * оба канала неисправны одновременно, переключение всё равно разрешаем
     * (иначе застреваем на одном канале навсегда без возможности хоть
     * что-то увидеть по второму; в этом случае управление всё равно
     * заблокировано на обоих, так что переключение безвредно). */
    if (ev->mask == BUTTON_MASK(BUTTON_TOOLS) && ev->type == BUTTON_EVENT_SHORT_PRESS) {
        channel_id_t target = (s_active_channel == CHANNEL_SOLDER) ? CHANNEL_DESOLDER : CHANNEL_SOLDER;
        if (!Error_IsChannelBlocked(target) || Error_IsChannelBlocked(s_active_channel)) {
            s_active_channel = target;
            s_accel.active = false; /* на всякий случай, физически невозможно при живом accel, но дёшево подстраховаться */
        }
        return;
    }

    if (s_screen_mode == SCREEN_MODE_SERVICE) {
        /* Глобальный выход из ЛЮБОГО уровня меню сразу в главный экран —
         * перехватывается здесь, ДО передачи в Menu (Menu эти комбинации
         * никогда не видит). Запись в EEPROM форсируется немедленно, не
         * дожидаясь обычного отложенного таймера Settings_Poll(). */
        bool global_exit =
            (ev->type == BUTTON_EVENT_CHORD_LONG && ev->mask == BUTTONS_CHORD_UP_DN_MASK) ||
            (ev->type == BUTTON_EVENT_SHORT_PRESS &&
             (ev->mask == BUTTON_MASK(BUTTON_SET1) || ev->mask == BUTTON_MASK(BUTTON_SET3)));

        if (global_exit) {
            Settings_Save();
            s_screen_mode = SCREEN_MODE_MAIN;
            return;
        }

        if (Menu_HandleEvent(ev) == MENU_ACTION_EXIT_TO_MAIN) {
            /* Пункт "Выход" уровня User — тот же форсированный Settings_Save() */
            Settings_Save();
            s_screen_mode = SCREEN_MODE_MAIN;
        }
        return;
    }

    /* Активный канал неисправен — управление (SET/UP/DN) заблокировано,
     * TOOLS (уже обработан выше) и UP+DN аккорд (выключение/сервисное меню)
     * под этот блок не подпадают. */
    if (Error_IsChannelBlocked(s_active_channel) && ev->mask != BUTTONS_CHORD_UP_DN_MASK) {
        return;
    }

    /* --- SET1/SET2/SET3 --- */
    if (ev->mask == BUTTON_MASK(BUTTON_SET1) ||
        ev->mask == BUTTON_MASK(BUTTON_SET2) ||
        ev->mask == BUTTON_MASK(BUTTON_SET3)) {

        button_id_t btn = (ev->mask == BUTTON_MASK(BUTTON_SET1)) ? BUTTON_SET1 :
                           (ev->mask == BUTTON_MASK(BUTTON_SET2)) ? BUTTON_SET2 : BUTTON_SET3;
        preset_id_t preset = preset_for_button(btn);

        if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
            apply_target_and_sync(s_active_channel, Settings_GetPreset(s_active_channel, preset));
        } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
            fixed_t cur = State_GetCurrentTemp(s_active_channel);
            int32_t cur_int = FIXED_TO_INT(cur);
            if (cur_int < 0) cur_int = 0; /* защита от аномального/отрицательного чтения датчика */
            Settings_SetPreset(s_active_channel, preset, (uint16_t)cur_int); /* клампится 50..450 внутри */
        }
        return;
    }

    /* --- UP / DN одиночные (не аккорд) --- */
    if (ev->mask == BUTTON_MASK(BUTTON_UP) || ev->mask == BUTTON_MASK(BUTTON_DN)) {
        button_id_t btn = (ev->mask == BUTTON_MASK(BUTTON_UP)) ? BUTTON_UP : BUTTON_DN;
        int32_t sign = (btn == BUTTON_UP) ? 1 : -1;

        if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
            StepAccel_ApplyDelta(accel_get, accel_set, NULL, sign, 1);
        } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
            accel_start(btn);
        }
        return;
    }

    /* --- UP+DN аккорд --- */
    if (ev->mask == BUTTONS_CHORD_UP_DN_MASK) {
        if (ev->type == BUTTON_EVENT_CHORD_SHORT) {
            bool new_enabled = !State_IsEnabled(s_active_channel);
            State_SetEnabled(s_active_channel, new_enabled);
            if (new_enabled) {
                /* Включение канала — считаем это активностью и для Sleep,
                 * иначе включённый инструмент может тут же оказаться в
                 * PRESLEEP/SLEEP, если простаивал ещё до включения. */
                Sleep_ForceAwake(s_active_channel);
            }
        } else if (ev->type == BUTTON_EVENT_CHORD_LONG) {
            s_screen_mode = SCREEN_MODE_SERVICE;
            Menu_Init(); /* всегда с чистого состояния: уровень User, курсор на первом пункте */
        }
        return;
    }

    /* BUTTON_EVENT_VIOLATION и прочее — игнорируем молча, см. докстринг */
}

/* ---- Публичный API ---- */

void InputFSM_Init(void)
{
    s_active_channel = CHANNEL_SOLDER;
    s_screen_mode = SCREEN_MODE_MAIN;

    StepAccel_Stop(&s_accel);
}

void InputFSM_SyncStateFromSettings(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint16_t target = Settings_GetTarget((channel_id_t)ch);
        State_SetSetpointTemp((channel_id_t)ch, FIXED_FROM_INT(target));
    }
}

void InputFSM_Poll(void)
{
    button_event_t ev;
    while (Buttons_PopEvent(&ev)) {
        dispatch_event(&ev);
    }

    if (s_screen_mode == SCREEN_MODE_SERVICE) {
        Menu_Poll();
        return; /* авто-повтор UP/DN главного экрана (ниже) не имеет смысла в меню */
    }

    if (s_accel.active) {
        if (!Buttons_IsHeld(s_accel.button) || Error_IsChannelBlocked(s_active_channel)) {
            s_accel.active = false;
        } else {
            StepAccel_Tick(&s_accel, HAL_GetTick());
        }
    }
}

channel_id_t InputFSM_GetActiveChannel(void)
{
    return s_active_channel;
}

screen_mode_t InputFSM_GetScreenMode(void)
{
    return s_screen_mode;
}
