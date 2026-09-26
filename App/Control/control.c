/**
 * @file control.c
 * @brief Реализация control.h — см. правила и обоснования в шапке заголовка.
 */

#include "control.h"
#include "channel.h"
#include "fixed_point.h"
#include "state.h"
#include "settings.h"
#include "sleep.h"
#include "error.h"
#include "ads1220.h"
#include "main.h"          /* Solder_On/Desolder_On — GPIO порт/пин */
#include "stm32f4xx_hal.h"

typedef struct {
    GPIO_TypeDef *on_port;
    uint16_t      on_pin;
} control_pins_t;

static const control_pins_t s_pins[CHANNEL_COUNT] = {
    [CHANNEL_SOLDER]   = { Solder_On_GPIO_Port,   Solder_On_Pin },
    [CHANNEL_DESOLDER] = { Desolder_On_GPIO_Port, Desolder_On_Pin },
};

/* Интеграл ошибки, Q16.16, "градус*секунда". Копится только в полосе
 * CONTROL_INTEGRAL_BAND_C вокруг уставки, вне полосы — 0 (см. control.h). */
static fixed_t  s_integral[CHANNEL_COUNT];

/* База для производной — температура и тик предыдущего пересчёта. */
static fixed_t  s_prev_temp[CHANNEL_COUNT];
static bool     s_prev_temp_valid[CHANNEL_COUNT];
static uint32_t s_last_update_tick[CHANNEL_COUNT];

/* Сглаженная dT/dt, °C/с (см. CONTROL_DTDT_FILTER_MS). */
static fixed_t  s_dTdt[CHANNEL_COUNT];
static bool     s_dTdt_valid[CHANNEL_COUNT];

/* Текущая мощность канала, %, 0..100 — выход PID, обновляется на новый
 * отсчёт АЦП; фазу ШИМ выходная ступень пересчитывает на каждый опрос. */
static uint8_t  s_duty_pct[CHANNEL_COUNT];

static void heater_write(channel_id_t ch, bool on)
{
    /* Нагреватель включается НИЗКИМ уровнем (см. diag.c). */
    HAL_GPIO_WritePin(s_pins[ch].on_port, s_pins[ch].on_pin,
                       on ? GPIO_PIN_RESET : GPIO_PIN_SET);
    State_SetHeaterActive(ch, on);
}

/* Программный ШИМ (time-proportional): включено первые duty% окна
 * CONTROL_PWM_PERIOD_MS. Фаза второго канала сдвинута на полпериода — оба
 * нагревателя не включаются одновременно. При переходе на аппаратный ШИМ
 * меняется только эта функция. */
static void pwm_stage(channel_id_t ch)
{
    uint32_t period = CONTROL_PWM_PERIOD_MS;
    uint32_t phase  = (HAL_GetTick() + (uint32_t)ch * (period / 2U)) % period;
    uint32_t on_ms  = ((uint32_t)s_duty_pct[ch] * period) / 100U;
    bool on = (phase < on_ms);   /* duty=0 -> всегда off, duty=100 -> всегда on */

    if (on != State_IsHeaterActive(ch)) {
        heater_write(ch, on);
    }
}

/* Принудительно выключить канал и сбросить состояние PID — общая точка
 * для всех "запрещающих нагрев" условий (fault/disabled/sleep/нет данных),
 * см. control.h. Идемпотентно — можно звать каждый опрос без разбора
 * переходов состояний. */
static void force_off(channel_id_t ch)
{
    s_duty_pct[ch] = 0;
    heater_write(ch, false);
    s_integral[ch] = 0;
    s_prev_temp_valid[ch] = false;
    s_dTdt_valid[ch] = false;
}

void Control_Init(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        channel_id_t ch = (channel_id_t)i;
        s_last_update_tick[ch] = 0;
        force_off(ch);
    }
}

fixed_t Control_PresleepSetpoint(channel_id_t ch, fixed_t setpoint)
{
    fixed_t presleep = FIXED_FROM_INT(Settings_GetPresleepTemp(ch));
    return (presleep < setpoint) ? presleep : setpoint;
}

static bool effective_setpoint(channel_id_t ch, fixed_t *out_setpoint)
{
    if (!State_IsEnabled(ch)) {
        return false; /* канал выключен пользователем (аккорд UP+DN, см. fsm.h) */
    }

    sleep_mode_t mode = Sleep_GetMode(ch);
    if (mode == SLEEP_MODE_SLEEP) {
        return false; /* см. sleep.h: SLEEP — нагрев отключён, решение уровня Control */
    }

    if (mode == SLEEP_MODE_PRESLEEP) {
        /* см. sleep.h: рабочая уставка (State) не меняется, эффективная —
         * min(уставка, Settings_GetPresleepTemp()), это решение уровня
         * Control. min — режим ожидания не должен греть выше уставки. */
        *out_setpoint = Control_PresleepSetpoint(ch, State_GetSetpointTemp(ch));
    } else {
        *out_setpoint = State_GetSetpointTemp(ch);
    }
    return true;
}

/* Один шаг PID на новый отсчёт АЦП: обновляет s_integral и s_duty_pct.
 * dt_s > 0. Вычисления выхода — в int64: Kp до 100 %/°C при ошибке до
 * сотен градусов в Q16.16 не помещается в int32. */
static void pid_step(channel_id_t ch, fixed_t setpoint, fixed_t true_setpoint, fixed_t temp, fixed_t dt_s)
{
    fixed_t error = setpoint - temp;

    /* Анти-виндап и гашение при перелёте считаем против НАСТОЯЩЕЙ уставки,
     * а не against working setpoint текущей стадии. Иначе на стадии 1
     * (см. "ДВУХСТАДИЙНАЯ УСТАВКА" в control.h) полоса CONTROL_INTEGRAL_BAND_C
     * может открыться сразу на холодном старте, если комнатная температура
     * уже близка к промежуточному рубежу (setpoint - CONTROL_APPROACH_OFFSET_C)
     * — типичный случай при включении на невысокую уставку. Интеграл тогда
     * успевает упереться в i_max ещё до перехода на стадию 2 и остаётся
     * насыщенным весь последний отрезок подъёма, что и даёт перелёт. */
    fixed_t true_error = true_setpoint - temp;

    fixed_t kp = fixed_div(FIXED_FROM_INT(Settings_GetKp(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));
    fixed_t ki = fixed_div(FIXED_FROM_INT(Settings_GetKi(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));
    fixed_t kd = fixed_div(FIXED_FROM_INT(Settings_GetKd(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));

    /* Интеграл: только в полосе вокруг настоящей уставки (анти-виндап),
     * иначе сброс. Клампится так, чтобы Ki*I лежало в [0, 100] %. */
    fixed_t band = FIXED_FROM_INT(CONTROL_INTEGRAL_BAND_C);
    if (ki > 0 && true_error <= band && true_error >= -band) {
        fixed_t i_max = fixed_div(FIXED_FROM_INT(100), ki);
        fixed_t i = s_integral[ch] + fixed_mul(error, dt_s);
        if (i < 0)     i = 0;
        if (i > i_max) i = i_max;

        /* Асимметричное гашение при перелёте (идея из UniSolder PID_OVSGain):
         * пока true_error>=0 копим/клампим как обычно; как только
         * true_error<0 (уже перелетели настоящую уставку), потолок интеграла
         * падает пропорционально величине перелёта — на CONTROL_OVERSHOOT_GAIN
         * процентов i_max за каждый градус перелёта. При overshoot >=
         * i_max/CONTROL_OVERSHOOT_GAIN градусов потолок уходит в 0, интеграл
         * гасится мгновенно, а не обычным темпом Ki*dt. Не влияет на
         * поведение без перелёта. */
        if (true_error < 0) {
            fixed_t overshoot = -true_error; /* °C, >0 */
            fixed_t reduction = fixed_mul(overshoot, FIXED_FROM_INT(CONTROL_OVERSHOOT_GAIN));
            fixed_t ceiling = i_max - reduction;
            if (ceiling < 0) ceiling = 0;
            if (i > ceiling) i = ceiling;
        }

        s_integral[ch] = i;
    } else {
        s_integral[ch] = 0;
    }

    int64_t out = (((int64_t)kp * error) >> FIXED_SHIFT)
                + (((int64_t)ki * s_integral[ch]) >> FIXED_SHIFT)
                - (((int64_t)kd * s_dTdt[ch]) >> FIXED_SHIFT);

    int64_t max = (int64_t)FIXED_FROM_INT(100);
    if (out < 0)   out = 0;
    if (out > max) out = max;

    s_duty_pct[ch] = (uint8_t)FIXED_TO_INT((fixed_t)out + (FIXED_ONE >> 1)); /* округление */
}

static void poll_channel(channel_id_t ch)
{
    /* Нет валидного отсчёта АЦП — греть вслепую нельзя, fail-safe off. */
    if (!ADS1220_IsDataValid(ch)) {
        force_off(ch);
        return;
    }

    /* Авария (КЗ/обрыв RTD, обрыв нагревателя, БП, EEPROM и т.п.) —
     * см. error.h. Блокирует канал независимо от прочих условий. */
    if (Error_IsChannelBlocked(ch)) {
        force_off(ch);
        return;
    }

    fixed_t setpoint;
    if (!effective_setpoint(ch, &setpoint)) {
        force_off(ch);
        return;
    }

    fixed_t temp = ADS1220_GetTemperatureC(ch);
    State_SetCurrentTemp(ch, temp); /* Control пишет current_temp, см. state.h */

    uint32_t update_tick = ADS1220_GetLastUpdateTick(ch);
    bool has_new_sample = (update_tick != s_last_update_tick[ch]);

    if (has_new_sample) {
        /* Пересчитываем только на реально новый отсчёт АЦП (иначе dT=0
         * исказит dT/dt). Первому отсчёту после сброса базы нет — ждём. */
        if (s_prev_temp_valid[ch]) {
            uint32_t dt_ms = update_tick - s_last_update_tick[ch];
            if (dt_ms > 0) {
                fixed_t dt_s = fixed_div(FIXED_FROM_INT((int32_t)dt_ms), FIXED_FROM_INT(1000));
                fixed_t raw  = fixed_div(temp - s_prev_temp[ch], dt_s); /* °C/с */

                if (!s_dTdt_valid[ch]) {
                    s_dTdt[ch] = raw;
                    s_dTdt_valid[ch] = true;
                } else {
                    /* Экспоненциальный фильтр: alpha = dt / (tau + dt). */
                    fixed_t alpha = fixed_div(FIXED_FROM_INT((int32_t)dt_ms),
                                              FIXED_FROM_INT((int32_t)(CONTROL_DTDT_FILTER_MS + dt_ms)));
                    s_dTdt[ch] += fixed_mul(alpha, raw - s_dTdt[ch]);
                }

                /* Двухстадийная уставка (см. control.h): пока температура
                 * ниже промежуточного рубежа, PID работает против него, а
                 * не против настоящей уставки — чистая функция (temp,
                 * setpoint), без отдельного состояния. */
                fixed_t approach = setpoint - FIXED_FROM_INT(CONTROL_APPROACH_OFFSET_C);
                fixed_t working_setpoint = (temp < approach) ? approach : setpoint;

                pid_step(ch, working_setpoint, setpoint, temp, dt_s);
            }
        }

        s_prev_temp[ch]        = temp;
        s_prev_temp_valid[ch]  = true;
        s_last_update_tick[ch] = update_tick;
    }

    pwm_stage(ch);
}

void Control_Poll(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        poll_channel((channel_id_t)i);
    }
}
