/**
 * @file beep.c
 * @brief Реализация beep.h.
 */

#include "beep.h"
#include "main.h"          /* BEEP_Pin / BEEP_GPIO_Port */
#include "stm32f4xx_hal.h"

/** Всего полупериодов в сигнале: каждый импульс = "вкл" + "выкл". */
#define BEEP_ALARM_HALF_STEPS (BEEP_ALARM_PULSES * 2U)

static uint32_t s_steps_left;   /* сколько полупериодов осталось; 0 = молчим */
static uint32_t s_last_tick;
static bool     s_pin_on;

static void pin_write(bool on)
{
    /* Активный ВЫСОКИЙ, см. beep.h/gpio.c */
    HAL_GPIO_WritePin(BEEP_GPIO_Port, BEEP_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    s_pin_on = on;
}

void Beep_Init(void)
{
    s_steps_left = 0;
    s_last_tick  = HAL_GetTick();
    pin_write(false);
}

void Beep_Alarm(void)
{
    if (s_steps_left != 0) {
        return; /* уже играет — не накладываем, см. beep.h */
    }
    s_steps_left = BEEP_ALARM_HALF_STEPS;
    s_last_tick  = HAL_GetTick();
    pin_write(true); /* первый полупериод — сразу "вкл", без ожидания */
}

void Beep_Poll(void)
{
    if (s_steps_left == 0) {
        return;
    }

    if ((HAL_GetTick() - s_last_tick) < BEEP_ALARM_HALF_PERIOD_MS) {
        return;
    }

    /* Без дрейфа: += period, а не = HAL_GetTick() — тот же принцип, что у
     * тайминг-гейтов в main.c. */
    s_last_tick += BEEP_ALARM_HALF_PERIOD_MS;
    s_steps_left--;

    if (s_steps_left == 0) {
        pin_write(false); /* гарантированно гасим в конце */
        return;
    }
    pin_write(!s_pin_on);
}

bool Beep_IsActive(void)
{
    return s_steps_left != 0;
}
