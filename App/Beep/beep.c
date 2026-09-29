/**
 * @file beep.c
 * @brief Реализация beep.h.
 */

#include "beep.h"
#include "main.h"          /* BEEP_Pin / BEEP_GPIO_Port */
#include "stm32f4xx_hal.h"
#include "settings.h"      /* Settings_GetFlagBit(SETTINGS_FLAG_BUZZER_BIT) — см. Beep_Alarm() */

/** Всего полупериодов в сигнале: каждый импульс = "вкл" + "выкл". */
#define BEEP_ALARM_HALF_STEPS (BEEP_ALARM_PULSES * 2U)

static uint32_t s_steps_left;     /* сколько шагов осталось; 0 = молчим */
static uint32_t s_step_ms;        /* длительность ТЕКУЩЕГО шага (полупериод меандра — для Alarm,
                                    * либо вся длительность одиночного тона — для PowerOn/Sleep) */
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

/**
 * @brief Общий вход для всех сигналов: проверка флага "Bzzz" и занятости,
 *        затем запуск s_steps_left шагов длительностью step_ms каждый.
 * @return true — запущен либо намеренно подавлен флагом; false — занято
 *         (предыдущий сигнал ещё не доигран), ничего не изменено.
 */
static bool start(uint32_t steps, uint32_t step_ms)
{
    /* Пункт меню "Bzzz" (см. menu.c/settings.h): при выключенном флаге
     * сигнал не играет. Единственная точка входа в модуль — проверка здесь. */
    if (!Settings_GetFlagBit(SETTINGS_FLAG_BUZZER_BIT)) {
        return true; /* намеренно подавлен — повторять запрос не нужно */
    }
    if (s_steps_left != 0) {
        return false; /* уже играет — не накладываем, см. beep.h; вызывающий повторит позже (если ему это важно) */
    }
    s_steps_left = steps;
    s_step_ms    = step_ms;
    s_last_tick  = HAL_GetTick();
    pin_write(true); /* первый шаг — сразу "вкл", без ожидания */
    return true;
}

bool Beep_Alarm(void)
{
    return start(BEEP_ALARM_HALF_STEPS, BEEP_ALARM_HALF_PERIOD_MS);
}

void Beep_PowerOn(void)
{
    /* Одиночный шаг = один непрерывный тон заданной длительности: после
     * него s_steps_left станет 0 и Beep_Poll() выключит пин, без второго
     * полупериода "выкл" — в отличие от Alarm(), тут это не меандр. */
    (void)start(1U, BEEP_POWER_ON_MS);
}

void Beep_EnteredSleep(void)
{
    (void)start(1U, BEEP_ENTERED_SLEEP_MS);
}

void Beep_Poll(void)
{
    if (s_steps_left == 0) {
        return;
    }

    if ((HAL_GetTick() - s_last_tick) < s_step_ms) {
        return;
    }

    /* Без дрейфа: += period, а не = HAL_GetTick() — тот же принцип, что у
     * тайминг-гейтов в main.c. */
    s_last_tick += s_step_ms;
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
