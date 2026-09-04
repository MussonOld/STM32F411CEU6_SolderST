/**
 * @file step_accel.c
 * @brief Реализация step_accel.h — см. правила в шапке заголовка.
 */

#include "step_accel.h"

void StepAccel_ApplyDelta(step_accel_get_fn get, step_accel_set_fn set, void *ctx,
                           int32_t sign, uint16_t step)
{
    int32_t v = (int32_t)get(ctx) + sign * (int32_t)step;
    if (v < 0) v = 0;
    set(ctx, (uint16_t)v);
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

static void apply_phase_step(step_accel_t *sa)
{
    int32_t sign = (sa->button == BUTTON_UP) ? 1 : -1;
    uint16_t step;
    uint16_t phase_limit;
    uint16_t round_to;

    switch (sa->phase) {
        case STEP_ACCEL_PHASE_1:
            step = STEP_ACCEL_STEP_1;
            phase_limit = STEP_ACCEL_STEP1_ITERATIONS;
            round_to = 5;
            break;
        case STEP_ACCEL_PHASE_5:
            step = STEP_ACCEL_STEP_5;
            phase_limit = STEP_ACCEL_STEP5_ITERATIONS;
            round_to = 10;
            break;
        case STEP_ACCEL_PHASE_10:
        default:
            step = STEP_ACCEL_STEP_10;
            phase_limit = 0; /* без ограничения — держим шаг 10 до отпускания */
            round_to = 0;
            break;
    }

    StepAccel_ApplyDelta(sa->get, sa->set, sa->ctx, sign, step);

    if (phase_limit != 0) {
        sa->iteration++;
        if (sa->iteration >= phase_limit) {
            uint16_t value_after = sa->get(sa->ctx);
            uint16_t rounded = (sign > 0) ? round_up_to_multiple(value_after, round_to)
                                           : round_down_to_multiple(value_after, round_to);
            sa->set(sa->ctx, rounded);

            sa->phase = (sa->phase == STEP_ACCEL_PHASE_1) ? STEP_ACCEL_PHASE_5 : STEP_ACCEL_PHASE_10;
            sa->iteration = 0;
        }
    }
}

void StepAccel_Start(step_accel_t *sa, button_id_t btn,
                      step_accel_get_fn get, step_accel_set_fn set, void *ctx,
                      uint32_t now)
{
    sa->active    = true;
    sa->button    = btn;
    sa->phase     = STEP_ACCEL_PHASE_1;
    sa->iteration = 0;
    sa->last_tick = now;
    sa->get       = get;
    sa->set       = set;
    sa->ctx       = ctx;
    apply_phase_step(sa); /* первый шаг сразу, не дожидаясь интервала */
}

void StepAccel_Stop(step_accel_t *sa)
{
    sa->active    = false;
    sa->phase     = STEP_ACCEL_PHASE_1;
    sa->iteration = 0;
}

void StepAccel_Tick(step_accel_t *sa, uint32_t now)
{
    if (!sa->active) return;
    if ((now - sa->last_tick) < STEP_ACCEL_REPEAT_MS) return;
    apply_phase_step(sa);
    sa->last_tick = now;
}
