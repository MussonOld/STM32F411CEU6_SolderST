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
#include "fixed_point.h"
#include "stm32f4xx_hal.h" /* HAL_GetTick() — интервал авто-повтора UP/DN */

/* ---- Тайминги/шаги авто-повтора UP/DN ----
 * Интервал между шагами при удержании — ВРЕМЕННЫЙ, не уточнялся, подобрать
 * по ощущениям на реальном железе. */
#define FSM_ACCEL_REPEAT_MS         (150U)
#define FSM_ACCEL_STEP1_ITERATIONS  (10U)
#define FSM_ACCEL_STEP5_ITERATIONS  (5U)
#define FSM_STEP_1   (1U)
#define FSM_STEP_5   (5U)
#define FSM_STEP_10  (10U)

typedef enum {
    ACCEL_PHASE_STEP1 = 0,
    ACCEL_PHASE_STEP5,
    ACCEL_PHASE_STEP10
} accel_phase_t;

static channel_id_t s_active_channel;
static screen_mode_t s_screen_mode;

static bool          s_accel_active;
static button_id_t   s_accel_button;
static accel_phase_t s_accel_phase;
static uint8_t       s_accel_iteration;
static uint32_t      s_accel_last_tick;

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
 * @brief Применить один шаг авто-повтора UP/DN, продвинуть фазу ускорения
 */
static void accel_apply_step(void)
{
    int32_t sign = (s_accel_button == BUTTON_UP) ? 1 : -1;
    uint16_t step;
    uint16_t phase_limit;
    uint16_t round_to;

    switch (s_accel_phase) {
        case ACCEL_PHASE_STEP1:
            step = FSM_STEP_1;
            phase_limit = FSM_ACCEL_STEP1_ITERATIONS;
            round_to = 5;
            break;
        case ACCEL_PHASE_STEP5:
            step = FSM_STEP_5;
            phase_limit = FSM_ACCEL_STEP5_ITERATIONS;
            round_to = 10;
            break;
        case ACCEL_PHASE_STEP10:
        default:
            step = FSM_STEP_10;
            phase_limit = 0; /* без ограничения — держим шаг 10 до отпускания */
            round_to = 0;
            break;
    }

    uint16_t cur = Settings_GetTarget(s_active_channel);
    apply_target_and_sync(s_active_channel, (int32_t)cur + sign * (int32_t)step);

    if (phase_limit != 0) {
        s_accel_iteration++;
        if (s_accel_iteration >= phase_limit) {
            uint16_t value_after = Settings_GetTarget(s_active_channel);
            uint16_t rounded = (sign > 0) ? round_up_to_multiple(value_after, round_to)
                                           : round_down_to_multiple(value_after, round_to);
            apply_target_and_sync(s_active_channel, rounded);

            s_accel_phase = (s_accel_phase == ACCEL_PHASE_STEP1) ? ACCEL_PHASE_STEP5 : ACCEL_PHASE_STEP10;
            s_accel_iteration = 0;
        }
    }
}

static void accel_start(button_id_t btn)
{
    s_accel_active    = true;
    s_accel_button     = btn;
    s_accel_phase      = ACCEL_PHASE_STEP1;
    s_accel_iteration  = 0;
    s_accel_last_tick  = HAL_GetTick();
    accel_apply_step(); /* первый шаг сразу, не дожидаясь интервала */
}

/**
 * @brief Разобрать одно событие от Buttons и применить действие
 */
static void dispatch_event(const button_event_t *ev)
{
    if (s_screen_mode == SCREEN_MODE_SERVICE) {
        /* Сервисное меню не специфицировано — заглушка, события пока не
         * обрабатываются. TODO: реализовать, когда меню будет описано. */
        return;
    }

    /* --- TOOLS: переключение активного канала --- */
    if (ev->mask == BUTTON_MASK(BUTTON_TOOLS) && ev->type == BUTTON_EVENT_SHORT_PRESS) {
        s_active_channel = (s_active_channel == CHANNEL_SOLDER) ? CHANNEL_DESOLDER : CHANNEL_SOLDER;
        s_accel_active = false; /* на всякий случай, физически невозможно при живом accel, но дёшево подстраховаться */
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
            uint16_t cur = Settings_GetTarget(s_active_channel);
            apply_target_and_sync(s_active_channel, (int32_t)cur + sign);
        } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
            accel_start(btn);
        }
        return;
    }

    /* --- UP+DN аккорд --- */
    if (ev->mask == BUTTONS_CHORD_UP_DN_MASK) {
        if (ev->type == BUTTON_EVENT_CHORD_SHORT) {
            State_SetEnabled(s_active_channel, false);
        } else if (ev->type == BUTTON_EVENT_CHORD_LONG) {
            s_screen_mode = SCREEN_MODE_SERVICE; /* заглушка — см. докстринг файла */
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

    s_accel_active = false;
    s_accel_button = BUTTON_UP;
    s_accel_phase = ACCEL_PHASE_STEP1;
    s_accel_iteration = 0;
    s_accel_last_tick = 0;
}

void InputFSM_Poll(void)
{
    button_event_t ev;
    while (Buttons_PopEvent(&ev)) {
        dispatch_event(&ev);
    }

    if (s_accel_active) {
        if (!Buttons_IsHeld(s_accel_button)) {
            s_accel_active = false;
        } else {
            uint32_t now = HAL_GetTick();
            if ((now - s_accel_last_tick) >= FSM_ACCEL_REPEAT_MS) {
                accel_apply_step();
                s_accel_last_tick = now;
            }
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
