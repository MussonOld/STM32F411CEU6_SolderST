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
#include "diag.h"
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

/* Сглаженная копия s_duty_pct для гейджа на экране (см.
 * Control_GetSmoothedPowerPct()/control.h) — НЕ используется в самом PID.
 * Q16.16, 0..100. */
static fixed_t  s_duty_smoothed[CHANNEL_COUNT];

/* Температура сейчас у настоящей уставки (|ошибка| <= CONTROL_POWER_DISPLAY_STEADY_BAND_C) —
 * переключает фильтр гейджа на медленный. Только для индикации. */
static bool     s_near_setpoint[CHANNEL_COUNT];

static void heater_write(channel_id_t ch, bool on)
{
    /* Нагреватель включается НИЗКИМ уровнем (см. diag.c). */
    HAL_GPIO_WritePin(s_pins[ch].on_port, s_pins[ch].on_pin,
                       on ? GPIO_PIN_RESET : GPIO_PIN_SET);
    State_SetHeaterActive(ch, on);
}

/* Программный ШИМ (time-proportional): включено первые duty% окна
 * CONTROL_PWM_PERIOD_MS. Фаза второго канала сдвинута на полпериода —
 * гарантированно без пересечения только пока оба duty <= 50%; выше не
 * подстраивается под фактические duty, возможно частичное перекрытие
 * (см. Control.md). При переходе на аппаратный ШИМ меняется только эта
 * функция. */
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
    s_near_setpoint[ch] = false;
}

#if CONTROL_LOG_ENABLE
/* Формат лога — менять только вместе с tools/control_log_decode.py.
 * Заголовок 32 байта + CONTROL_LOG_CAPACITY записей по 20 байт, little-endian.
 * Единицы: температуры — °C*16 (int16, >>12 от Q16.16), слагаемые PID и dT/dt —
 * значение*10 (%, °C/с), duty — целые %. */
typedef struct __attribute__((packed)) {
    uint16_t t_ms;        /* мс от начала лога */
    int16_t  temp_q4;     /* измеренная температура, °C*16 */
    int16_t  work_sp_q4;  /* рабочая уставка PID (с учётом стадии подхода), °C*16 */
    int16_t  true_sp_q4;  /* настоящая уставка, °C*16 */
    int16_t  p_x10;       /* Kp*error, %*10 */
    int16_t  i_x10;       /* Ki*integral, %*10 */
    int16_t  d_x10;       /* -Kd*dT/dt, %*10 */
    int16_t  ff_x10;      /* feed-forward, %*10 */
    int16_t  dtdt_x10;    /* dT/dt после фильтра, °C/с*10 */
    uint8_t  duty;        /* выход, % */
    uint8_t  flags;       /* bit0: стадия подхода (рабочая уставка < настоящей) */
} control_log_rec_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* 0x474F4C53 "SLOG" */
    uint16_t version;     /* 1 */
    uint16_t rec_size;    /* sizeof(control_log_rec_t) */
    uint32_t capacity;
    uint32_t count;       /* сколько записей валидно */
    uint32_t start_tick;  /* HAL_GetTick() в начале лога */
    uint32_t reserved[3];
} control_log_hdr_t;

typedef struct __attribute__((packed)) {
    control_log_hdr_t   hdr;
    control_log_rec_t   rec[CONTROL_LOG_CAPACITY];
} control_log_t;

_Static_assert(sizeof(control_log_rec_t) == 20, "log record layout");
_Static_assert(sizeof(control_log_hdr_t) == 32, "log header layout");
_Static_assert(sizeof(control_log_t) <= 24576U, "log must fit the 24 KB LOG region in the linker script");

__attribute__((section(".control_log"), used)) control_log_t s_log;
static fixed_t  s_log_setpoint;
static uint32_t s_log_last_tick;
static bool     s_log_started;

static int16_t log_clamp16(int64_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}
static int16_t log_x10(int64_t q16)  { return log_clamp16((q16 * 10) >> FIXED_SHIFT); }
static int16_t log_q4(fixed_t q16)   { return log_clamp16((int64_t)q16 >> 12); }

static void log_reset(void)
{
    s_log.hdr.magic      = 0x474F4C53UL;
    s_log.hdr.version    = 1U;
    s_log.hdr.rec_size   = (uint16_t)sizeof(control_log_rec_t);
    s_log.hdr.capacity   = CONTROL_LOG_CAPACITY;
    s_log.hdr.count      = 0U;
    s_log.hdr.start_tick = HAL_GetTick();
}

/* Одна запись на новый отсчёт; старт нового лога — смена уставки или пауза. */
static void log_sample(channel_id_t ch, fixed_t work_sp, fixed_t true_sp, fixed_t temp,
                       fixed_t p, fixed_t i, fixed_t d, fixed_t ff, uint8_t duty)
{
    if (ch != CONTROL_LOG_CHANNEL) return;
    uint32_t now = HAL_GetTick();
    if (!s_log_started || true_sp != s_log_setpoint
        || (uint32_t)(now - s_log_last_tick) > CONTROL_LOG_GAP_MS) {
        log_reset();
        s_log_started  = true;
        s_log_setpoint = true_sp;
    }
    s_log_last_tick = now;
    uint32_t n = s_log.hdr.count;
    if (n >= CONTROL_LOG_CAPACITY) return;
    control_log_rec_t *r = &s_log.rec[n];
    r->t_ms       = (uint16_t)(now - s_log.hdr.start_tick);
    r->temp_q4    = log_q4(temp);
    r->work_sp_q4 = log_q4(work_sp);
    r->true_sp_q4 = log_q4(true_sp);
    r->p_x10      = log_x10(p);
    r->i_x10      = log_x10(i);
    r->d_x10      = log_x10(d);
    r->ff_x10     = log_x10(ff);
    r->dtdt_x10   = log_x10(s_dTdt[ch]);
    r->duty       = duty;
    r->flags      = (work_sp < true_sp) ? 1U : 0U;
    s_log.hdr.count = n + 1U; /* последним: читатель видит только готовые записи */
}
#endif /* CONTROL_LOG_ENABLE */

void Control_Init(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        channel_id_t ch = (channel_id_t)i;
        s_last_update_tick[ch] = 0;
        s_duty_smoothed[ch] = 0;
        force_off(ch);
    }
#if CONTROL_LOG_ENABLE
    log_reset(); /* валидный заголовок с count=0 сразу после старта МК */
#endif
}

/* Экспоненциальный фильтр s_duty_pct -> s_duty_smoothed для гейджа на
 * экране (см. control.h). Вызывается КАЖДЫЙ Control_Poll(), с фиксированным
 * dt=CONTROL_POLL_MS — в отличие от dT/dt (привязан к реальным отсчётам
 * АЦП), тут сглаживать нужно именно "мгновенную" мощность, включая
 * периоды между отсчётами АЦП, где s_duty_pct не меняется, но и не должен
 * считаться "новым" значением — фиксированный шаг делает фильтр корректным
 * time-domain LPF независимо от темпа АЦП. */
static void update_power_smoothing(channel_id_t ch)
{
    fixed_t raw = FIXED_FROM_INT((int32_t)s_duty_pct[ch]);
    uint32_t tau_ms = s_near_setpoint[ch] ? CONTROL_POWER_DISPLAY_FILTER_STEADY_MS
                                          : CONTROL_POWER_DISPLAY_FILTER_MS;
    fixed_t alpha = fixed_div(FIXED_FROM_INT((int32_t)CONTROL_POLL_MS),
                              FIXED_FROM_INT((int32_t)(tau_ms + CONTROL_POLL_MS)));
    s_duty_smoothed[ch] += fixed_mul(alpha, raw - s_duty_smoothed[ch]);
}

fixed_t Control_GetSmoothedPowerPct(channel_id_t ch)
{
    return s_duty_smoothed[ch];
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

/* Коэффициент PID из Settings (целое, сотые доли) в Q16.16: raw / CONTROL_PID_SCALE. */
static fixed_t pid_gain(uint16_t raw)
{
    return fixed_div(FIXED_FROM_INT(raw), FIXED_FROM_INT(CONTROL_PID_SCALE));
}

/* Асимметричное гашение интеграла при перелёте (идея из UniSolder PID_OVSGain):
 * пока true_error>=0 копим/клампим как обычно; как только true_error<0 (уже
 * перелетели настоящую уставку), потолок интеграла падает пропорционально
 * величине перелёта — на CONTROL_OVERSHOOT_GAIN процентов i_max за каждый
 * градус перелёта. При overshoot >= i_max/CONTROL_OVERSHOOT_GAIN градусов
 * потолок уходит в 0, интеграл гасится мгновенно, а не обычным темпом Ki*dt.
 * Не влияет на поведение без перелёта. */
static fixed_t limit_integral_on_overshoot(fixed_t i, fixed_t i_max, fixed_t true_error)
{
    if (true_error >= 0) {
        return i;
    }
    fixed_t overshoot = -true_error; /* °C, >0 */
    fixed_t reduction = fixed_mul(overshoot, FIXED_FROM_INT(CONTROL_OVERSHOOT_GAIN));
    fixed_t ceiling = i_max - reduction;
    if (ceiling < 0) ceiling = 0;
    return (i > ceiling) ? ceiling : i;
}

/* Интеграл: только в полосе вокруг настоящей уставки (анти-виндап), иначе
 * сброс. Клампится так, чтобы Ki*I лежало в [0, 100] %. */
static void update_integral(channel_id_t ch, fixed_t error, fixed_t true_error, fixed_t ki, fixed_t dt_s)
{
    fixed_t band = FIXED_FROM_INT(CONTROL_INTEGRAL_BAND_C);
    if (!(ki > 0 && true_error <= band && true_error >= -band)) {
        s_integral[ch] = 0;
        return;
    }

    fixed_t i_max = fixed_div(FIXED_FROM_INT(100), ki);
    fixed_t i = s_integral[ch] + fixed_mul(error, dt_s);
    if (i < 0)     i = 0;
    if (i > i_max) i = i_max;

    s_integral[ch] = limit_integral_on_overshoot(i, i_max, true_error);
}

/* Прямая подача на удержание (см. CONTROL_FF_PCT_PER_100C): только в полосе
 * CONTROL_INTEGRAL_BAND_C вокруг настоящей уставки, вне неё 0. Q16.16, %. */
static fixed_t feed_forward_pct(fixed_t true_setpoint, fixed_t true_error, fixed_t dTdt)
{
    fixed_t band = FIXED_FROM_INT(CONTROL_INTEGRAL_BAND_C);
    if (true_error > band || true_error < -band) {
        return 0;
    }
    fixed_t rise = true_setpoint - FIXED_FROM_INT(CONTROL_AMBIENT_C);
    if (rise <= 0) {
        return 0;
    }
    fixed_t ff = fixed_div(fixed_mul(FIXED_FROM_INT(CONTROL_FF_PCT_PER_100C), rise), FIXED_FROM_INT(100));

    /* Пока температура ещё заметно растёт — FF линейно убирается (см.
     * CONTROL_FF_FADE_DTDT_C_PER_S): тепло от нагревателя к жалу и так догоняет. */
    fixed_t fade = FIXED_FROM_INT(CONTROL_FF_FADE_DTDT_C_PER_S);
    if (dTdt <= 0) {
        return ff;
    }
    if (dTdt >= fade) {
        return 0;
    }
    return fixed_mul(ff, fixed_div(fade - dTdt, fade));
}

/* Выход PID в процентах 0..100 (с округлением). Вычисления — в int64: Kp до
 * 100 %/°C при ошибке до сотен градусов в Q16.16 не помещается в int32. */
static uint8_t pid_output_pct(fixed_t kp, fixed_t ki, fixed_t kd, fixed_t error, fixed_t integral, fixed_t dTdt, fixed_t ff)
{
    int64_t out = (((int64_t)kp * error) >> FIXED_SHIFT)
                + (((int64_t)ki * integral) >> FIXED_SHIFT)
                - (((int64_t)kd * dTdt) >> FIXED_SHIFT)
                + (int64_t)ff;

    int64_t max = (int64_t)FIXED_FROM_INT(100);
    if (out < 0)   out = 0;
    if (out > max) out = max;

    return (uint8_t)FIXED_TO_INT((fixed_t)out + (FIXED_ONE >> 1)); /* округление */
}

/* Один шаг PID на новый отсчёт АЦП: обновляет s_integral и s_duty_pct.
 * dt_s > 0. */
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

    fixed_t kp = pid_gain(Settings_GetKp(ch));
    fixed_t ki = pid_gain(Settings_GetKi(ch));
    fixed_t kd = pid_gain(Settings_GetKd(ch));

    fixed_t steady_band = FIXED_FROM_INT(CONTROL_POWER_DISPLAY_STEADY_BAND_C);
    s_near_setpoint[ch] = (true_error <= steady_band && true_error >= -steady_band);

    update_integral(ch, error, true_error, ki, dt_s);
    fixed_t ff = feed_forward_pct(true_setpoint, true_error, s_dTdt[ch]);
    s_duty_pct[ch] = pid_output_pct(kp, ki, kd, error, s_integral[ch], s_dTdt[ch], ff);
#if CONTROL_LOG_ENABLE
    log_sample(ch, setpoint, true_setpoint, temp,
               (fixed_t)(((int64_t)kp * error) >> FIXED_SHIFT),
               (fixed_t)(((int64_t)ki * s_integral[ch]) >> FIXED_SHIFT),
               (fixed_t)(-(((int64_t)kd * s_dTdt[ch]) >> FIXED_SHIFT)),
               ff, s_duty_pct[ch]);
#endif
}

/* Сглаженная dT/dt: первый отсчёт после сброса берётся как есть, дальше —
 * экспоненциальный фильтр alpha = dt / (tau + dt). dt_ms > 0. */
static void update_dTdt(channel_id_t ch, fixed_t temp, uint32_t dt_ms, fixed_t dt_s)
{
    fixed_t raw = fixed_div(temp - s_prev_temp[ch], dt_s); /* °C/с */

    if (!s_dTdt_valid[ch]) {
        s_dTdt[ch] = raw;
        s_dTdt_valid[ch] = true;
        return;
    }
    fixed_t alpha = fixed_div(FIXED_FROM_INT((int32_t)dt_ms),
                              FIXED_FROM_INT((int32_t)(CONTROL_DTDT_FILTER_MS + dt_ms)));
    s_dTdt[ch] += fixed_mul(alpha, raw - s_dTdt[ch]);
}

/* Двухстадийная уставка (см. control.h): пока температура ниже
 * промежуточного рубежа, PID работает против него, а не против настоящей
 * уставки — чистая функция (temp, setpoint), без отдельного состояния.
 *
 * Величину отступа масштабируем по факту оставшегося пути (setpoint - temp),
 * а не берём фиксированным потолком CONTROL_APPROACH_OFFSET_C напрямую: на
 * большом прыжке (остаток >> 2*OFFSET, напр. холодный старт на высокую
 * уставку) offset упирается в потолок. На малом остатке (холодный старт на
 * низкую уставку, где offset — заметная доля всего пути) offset сам
 * уменьшается и гладко идёт к 0 по мере приближения temp к setpoint. Без
 * этого при фиксированном offset в момент переключения стадии
 * working_setpoint скачком увеличивался на OFFSET (PID уже притормозил,
 * подходя к промежуточному рубежу, и тут же получал новый скачок error) — на
 * маленьких уставках это добавляло свежий разгон почти у финиша и давало
 * перелёт. */
static fixed_t working_setpoint_for(fixed_t setpoint, fixed_t temp)
{
    fixed_t remaining   = setpoint - temp; /* >0, пока не долетели */
    fixed_t offset_cap  = FIXED_FROM_INT(CONTROL_APPROACH_OFFSET_C);
    fixed_t offset      = remaining >> 1;
    if (offset > offset_cap) offset = offset_cap;
    if (offset < 0)          offset = 0;

    fixed_t approach = setpoint - offset;
    return (temp < approach) ? approach : setpoint;
}

/* Пересчёт только на реально новый отсчёт АЦП (иначе dT=0 исказит dT/dt).
 * Первому отсчёту после сброса базы нет — ждём. */
static void process_new_sample(channel_id_t ch, fixed_t setpoint, fixed_t temp, uint32_t update_tick)
{
    if (s_prev_temp_valid[ch]) {
        uint32_t dt_ms = update_tick - s_last_update_tick[ch];
        if (dt_ms > 0) {
            fixed_t dt_s = fixed_div(FIXED_FROM_INT((int32_t)dt_ms), FIXED_FROM_INT(1000));
            update_dTdt(ch, temp, dt_ms, dt_s);
            pid_step(ch, working_setpoint_for(setpoint, temp), setpoint, temp, dt_s);
        }
    }

    s_prev_temp[ch]        = temp;
    s_prev_temp_valid[ch]  = true;
    s_last_update_tick[ch] = update_tick;
}

/* Пригоден ли отсчёт АЦП: тогда температуре можно верить (показывать, считать
 * по ней PID), а если нет — канал fail-safe off. */
static bool measurement_usable(channel_id_t ch)
{
    /* Нет валидного отсчёта АЦП — греть вслепую нельзя, fail-safe off. Тот же
     * отказ, если Diag ещё не оценил ПОСЛЕДНИЙ отсчёт (иначе Error относился бы
     * к предыдущему измерению): при порядке ADS1220 -> Diag -> Control из main.c
     * не срабатывает никогда, но делает порядок проверяемым, а не негласным. */
    if (!ADS1220_IsDataValid(ch) || !Diag_IsSampleEvaluated(ch)) {
        return false;
    }

    /* Авария (КЗ/обрыв RTD, обрыв нагревателя, БП, EEPROM и т.п.) —
     * см. error.h. Блокирует канал независимо от прочих условий. */
    return !Error_IsChannelBlocked(ch);
}

static void poll_channel(channel_id_t ch)
{
    if (!measurement_usable(ch)) {
        force_off(ch);
        return;
    }

    /* Измеренная температура публикуется в State, пока отсчёт пригоден, — и
     * когда канал не греет (выключен аккордом UP+DN, SLEEP): экран показывает её
     * под иконкой сна, и она должна остывать вместе с жалом, а не замереть на
     * значении момента засыпания. Раньше запись стояла после проверки уставки
     * и в этих состояниях не выполнялась. */
    fixed_t temp = ADS1220_GetTemperatureC(ch);
    State_SetCurrentTemp(ch, temp); /* Control пишет current_temp, см. state.h */

    fixed_t setpoint;
    if (!effective_setpoint(ch, &setpoint)) {
        force_off(ch);
        return;
    }

    uint32_t update_tick = ADS1220_GetLastUpdateTick(ch);
    if (update_tick != s_last_update_tick[ch]) {
        process_new_sample(ch, setpoint, temp, update_tick);
    }

    pwm_stage(ch);
}

void Control_Poll(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        channel_id_t ch = (channel_id_t)i;
        poll_channel(ch);
        update_power_smoothing(ch); /* безусловно, даже если poll_channel() выше сделал force_off() — см. control.h */
    }
}
