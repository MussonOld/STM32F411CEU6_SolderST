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

/* Интеграл ошибки, накопленный за текущий цикл нагрева. Q16.16,
 * "градус*секунда". Обнуляется на каждом ВКЛЮЧЕНИИ (не переносится между
 * циклами) и клампится каждый пересчёт — см. control.h. Пока Ki=0 ни на
 * что не влияет, но должен считаться правильно уже сейчас. */
static fixed_t  s_integral[CHANNEL_COUNT];

/* База для производной — температура и тик предыдущего пересчёта. */
static fixed_t  s_prev_temp[CHANNEL_COUNT];
static bool     s_prev_temp_valid[CHANNEL_COUNT];
static uint32_t s_last_update_tick[CHANNEL_COUNT];

/* Последняя посчитанная dT/dt, °C/с. Обновляется на КАЖДЫЙ новый отсчёт АЦП —
 * и при нагреве (для выключения), и при выключенном нагревателе (для
 * прогноза при включении, см. predicted_below_setpoint()). */
static fixed_t  s_dTdt[CHANNEL_COUNT];
static bool     s_dTdt_valid[CHANNEL_COUNT];

/* Клампинг интеграла — величина сама по себе неважна, пока Ki=0; главное,
 * чтобы была КОНЕЧНОЙ, чтобы наладка Ki не унаследовала неограниченный
 * бэклог с прошлых прогонов. ВРЕМЕННО — подобрать вместе с Ki, см. чат. */
#define CONTROL_INTEGRAL_CLAMP FIXED_FROM_INT(1000)

static fixed_t clamp_fixed(fixed_t v, fixed_t lo, fixed_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void heater_write(channel_id_t ch, bool on)
{
    /* Нагреватель включается НИЗКИМ уровнем (см. diag.c). */
    HAL_GPIO_WritePin(s_pins[ch].on_port, s_pins[ch].on_pin,
                       on ? GPIO_PIN_RESET : GPIO_PIN_SET);
    State_SetHeaterActive(ch, on);
}

/* Принудительно выключить канал и сбросить состояние PID — общая точка
 * для всех "запрещающих нагрев" условий (fault/disabled/sleep/нет данных),
 * см. control.h. Идемпотентно — можно звать каждый опрос без разбора
 * переходов состояний. */
static void force_off(channel_id_t ch)
{
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

/* Прогноз для ВКЛЮЧЕНИЯ: греть имеет смысл, только если T + (Kd/Kp)*dT/dt
 * ещё ниже уставки — т.е. то же условие output > 0, что и на выключении
 * (без интеграла: он в начале цикла нагрева и так 0). Без этого упреждающее
 * выключение ниже порога гистерезиса тут же отменялось бы обратным
 * включением на следующем опросе. Kp == 0 (PID не настроен) — прогноза нет,
 * решает один гистерезис, как раньше. */
static bool predicted_below_setpoint(channel_id_t ch, fixed_t setpoint, fixed_t temp)
{
    fixed_t kp = fixed_div(FIXED_FROM_INT(Settings_GetKp(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));
    if (kp <= 0) {
        return true;
    }
    fixed_t kd    = fixed_div(FIXED_FROM_INT(Settings_GetKd(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));
    fixed_t dTdt  = s_dTdt_valid[ch] ? s_dTdt[ch] : 0;
    fixed_t output = fixed_mul(kp, setpoint - temp) - fixed_mul(kd, dTdt);
    return output > 0;
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

    /* dT/dt на каждый новый отсчёт (нужна и при нагреве, и при выключенном
     * нагревателе). dt_s == 0 — базы ещё нет, производной нет. */
    fixed_t dt_s = 0;
    if (has_new_sample && s_prev_temp_valid[ch]) {
        uint32_t dt_ms = update_tick - s_last_update_tick[ch];
        if (dt_ms > 0) {
            dt_s = fixed_div(FIXED_FROM_INT((int32_t)dt_ms), FIXED_FROM_INT(1000));
            fixed_t raw = fixed_div(temp - s_prev_temp[ch], dt_s); /* °C/с */
            if (!s_dTdt_valid[ch]) {
                s_dTdt[ch] = raw; /* первый отсчёт после сброса — без сглаживания */
            } else {
                /* Экспоненциальный фильтр: alpha = dt / (tau + dt). */
                fixed_t alpha = fixed_div(FIXED_FROM_INT((int32_t)dt_ms),
                                          FIXED_FROM_INT((int32_t)(CONTROL_DTDT_FILTER_MS + dt_ms)));
                s_dTdt[ch] += fixed_mul(alpha, raw - s_dTdt[ch]);
            }
            s_dTdt_valid[ch] = true;
        }
    }

    bool heating = State_IsHeaterActive(ch);

    if (!heating) {
        /* ВКЛЮЧЕНИЕ — гистерезис И прогноз (T + Kd/Kp*dT/dt < уставки), см.
         * predicted_below_setpoint(). */
        fixed_t threshold = setpoint - FIXED_FROM_INT(CONTROL_HYSTERESIS_C);
        if (temp <= threshold && predicted_below_setpoint(ch, setpoint, temp)) {
            heater_write(ch, true);
            s_integral[ch] = 0; /* новый цикл нагрева — без переноса интеграла, см. control.h */
            /* Базу производной не трогаем здесь: если есть валидная предыдущая
             * точка — она пригодится на следующем пересчёте; если нет — просто
             * подождём один отсчёт (см. ветку ниже). */
        }
    } else if (has_new_sample) {
        /* ВЫКЛЮЧЕНИЕ — момент определяет PID, см. control.h. Пересчитываем
         * только на реально новый отсчёт АЦП (иначе dT=0 исказит dT/dt). */
        if (dt_s > 0) {
            fixed_t error = setpoint - temp;
            fixed_t dTdt  = s_dTdt[ch];

            /* Анти-виндап: копим интеграл, только пока реально греем (сюда и
             * попадаем только пока heating==true); клампим независимо от Ki. */
            s_integral[ch] = clamp_fixed(s_integral[ch] + fixed_mul(error, dt_s),
                                          -CONTROL_INTEGRAL_CLAMP, CONTROL_INTEGRAL_CLAMP);

            fixed_t kp = fixed_div(FIXED_FROM_INT(Settings_GetKp(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));
            fixed_t ki = fixed_div(FIXED_FROM_INT(Settings_GetKi(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));
            fixed_t kd = fixed_div(FIXED_FROM_INT(Settings_GetKd(ch)), FIXED_FROM_INT(CONTROL_PID_SCALE));

            fixed_t output = fixed_mul(kp, error) + fixed_mul(ki, s_integral[ch]) - fixed_mul(kd, dTdt);

            if (output <= 0) {
                heater_write(ch, false);
                /* s_integral намеренно не обнуляем здесь — обнуление только на
                 * СЛЕДУЮЩЕМ включении (см. ветку выше), чтобы клампинг выше
                 * оставался корректен, даже если output посчитан на самой
                 * границе выключения. */
            }
        }
    }

    if (has_new_sample) {
        s_prev_temp[ch]       = temp;
        s_prev_temp_valid[ch] = true;
        s_last_update_tick[ch] = update_tick;
    }
}

void Control_Poll(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        poll_channel((channel_id_t)i);
    }
}
