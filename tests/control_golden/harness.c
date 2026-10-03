/*
 * Golden-master harness для App/Control/control.c (хост, gcc; в прошивку не входит).
 *
 * Замкнутый контур на хосте: control.c ПОДКЛЮЧАЕТСЯ ЦЕЛИКОМ (#include "control.c"), чтобы
 * трассировать не только выходы (запись пина нагревателя, State_SetHeaterActive,
 * State_SetCurrentTemp, Control_GetSmoothedPowerPct), но и внутреннее состояние регулятора
 * на каждом шаге: s_duty_pct, s_integral, s_dTdt, s_prev_temp, s_last_update_tick, флаги
 * валидности. Поэтому при рефакторинге эти static-переменные должны сохранять имена.
 *
 * Мир: простая тепловая модель каждого канала (нагрев по фактическому состоянию нагревателя,
 * остывание к комнатной, шум датчика) + случайные эпизоды: смена уставки, режимы сна
 * (AWAKE/PRESLEEP/SLEEP), выключение канала, авария/блокировка, невалидные данные АЦП,
 * непроверенный отсчёт Diag, разные Kp/Ki/Kd (в т.ч. 0 и максимум), "хаос" — случайные
 * температуры (полоса интеграла, перелёт, гашение), неравномерный темп АЦП, тик у переполнения.
 * Рефакторинг control.c обязан сохранить трассу побайтно на том же seed.
 *
 *   ./golden [steps] [seed]       -> RESULT-строка с итоговым хешем
 *   DUMP=N ./golden ...           -> текстовый дамп шага N
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "control.c"   /* white-box: статические переменные регулятора видны ниже */

static uint64_t g_hash = 1469598103934665603ULL;
static uint64_t g_events;
static long g_step;
static long g_dump_step = -2;

static void mix_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { g_hash ^= b[i]; g_hash *= 1099511628211ULL; }
}
static void ev(const char *name, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    mix_bytes(name, strlen(name));
    mix_bytes(buf, strlen(buf));
    g_events++;
    if (g_step == g_dump_step) printf("  %s(%s)\n", name, buf);
}

static uint64_t g_rng = 88172645463325252ULL;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
static bool chance(unsigned pct) { return (rnd() % 100U) < pct; }
static double frand(void) { return (double)(rnd() % 1000000U) / 1000000.0; }

/* ------------------------------------------------------------ модель мира */
static uint32_t g_tick;
GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc;

typedef struct {
    bool enabled, blocked, data_valid, evaluated, heater;
    sleep_mode_t sleep;
    fixed_t setpoint, meas_temp;
    uint16_t presleep_temp, kp, ki, kd;
    uint32_t update_tick;
    double plant;          /* "истинная" температура, °C */
    uint32_t next_sample;  /* когда ADS выдаст новый отсчёт */
    uint32_t sample_period;
    bool chaos;            /* измерение случайное, не из модели */
} world_ch_t;
static world_ch_t W[CHANNEL_COUNT];

uint32_t HAL_GetTick(void) { return g_tick; }
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState st) { ev("gpio", "%c,%u,%d", port == GPIOB ? 'B' : '?', pin, (int)st); } /* не печатаем адрес порта — ASLR сделал бы трассу недетерминированной */
void State_SetHeaterActive(channel_id_t ch, bool a) { W[ch].heater = a; ev("heater", "%d,%d", (int)ch, (int)a); }
bool State_IsHeaterActive(channel_id_t ch) { return W[ch].heater; }
bool State_IsEnabled(channel_id_t ch) { return W[ch].enabled; }
fixed_t State_GetSetpointTemp(channel_id_t ch) { return W[ch].setpoint; }
void State_SetCurrentTemp(channel_id_t ch, fixed_t t) { ev("curtemp", "%d,%d", (int)ch, (int)t); }
sleep_mode_t Sleep_GetMode(channel_id_t ch) { return W[ch].sleep; }
bool Error_IsChannelBlocked(channel_id_t ch) { return W[ch].blocked; }
bool Diag_IsSampleEvaluated(channel_id_t ch) { return W[ch].evaluated; }
bool ADS1220_IsDataValid(channel_id_t ch) { return W[ch].data_valid; }
fixed_t ADS1220_GetTemperatureC(channel_id_t ch) { return W[ch].meas_temp; }
uint32_t ADS1220_GetLastUpdateTick(channel_id_t ch) { return W[ch].update_tick; }
uint16_t Settings_GetPresleepTemp(channel_id_t ch) { return W[ch].presleep_temp; }
uint16_t Settings_GetKp(channel_id_t ch) { return W[ch].kp; }
uint16_t Settings_GetKi(channel_id_t ch) { return W[ch].ki; }
uint16_t Settings_GetKd(channel_id_t ch) { return W[ch].kd; }

static fixed_t to_fixed(double c) { return (fixed_t)(c * 65536.0); }

static void new_gains(world_ch_t *w)
{
    unsigned r = rnd() % 100U;
    if (r < 55) { w->kp = (uint16_t)(300 + rnd() % 3000U); w->ki = (uint16_t)(rnd() % 400U); w->kd = (uint16_t)(rnd() % 2500U); }
    else if (r < 65) { w->kp = 0; w->ki = 0; w->kd = 0; }
    else if (r < 75) { w->kp = 10000; w->ki = 10000; w->kd = 10000; }
    else if (r < 85) { w->ki = 0; w->kp = (uint16_t)(rnd() % 5000U); w->kd = (uint16_t)(rnd() % 5000U); }
    else if (r < 92) { w->kp = (uint16_t)(rnd() % 500U); w->ki = (uint16_t)(1 + rnd() % 5U); w->kd = 0; }  /* крошечный Ki: огромный i_max */
    else { w->kp = (uint16_t)rnd(); w->ki = (uint16_t)rnd(); w->kd = (uint16_t)rnd(); }
}

static void world_init(void)
{
    memset(W, 0, sizeof W);
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        W[c].enabled = true; W[c].data_valid = true; W[c].evaluated = true;
        W[c].plant = 25.0; W[c].setpoint = FIXED_FROM_INT(300 - 40 * c); W[c].presleep_temp = 150;
        W[c].sample_period = 20 + 7U * (unsigned)c; W[c].next_sample = g_tick + W[c].sample_period;
        W[c].update_tick = g_tick;
        W[c].meas_temp = to_fixed(W[c].plant);
        new_gains(&W[c]);
    }
}

static void world_step(uint32_t dt_ms)
{
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        world_ch_t *w = &W[c];
        /* тепловая модель: нагрев при включённом нагревателе, остывание к 25 °C */
        double dt = dt_ms / 1000.0;
        w->plant += (w->heater ? 55.0 : 0.0) * dt - (w->plant - 25.0) * 0.045 * dt;
        if (w->plant > 700.0) w->plant = 700.0;   /* физически осмысленный диапазон датчика: вне него Q16.16-разности переполнились бы */
        if (w->plant < -100.0) w->plant = -100.0;

        /* эпизоды */
        if (chance(1)) w->setpoint = FIXED_FROM_INT((int32_t)(100 + rnd() % 351U));
        if (chance(1)) w->setpoint = FIXED_FROM_INT((int32_t)(100 + rnd() % 351U)) + to_fixed(frand());   /* дробная уставка; диапазон как у Settings (50..450) */
        if (chance(1)) w->presleep_temp = (uint16_t)(chance(15) ? rnd() % 700U : 80 + rnd() % 200U);
        if (chance(1)) w->enabled = chance(85);
        if (chance(2)) w->sleep = chance(70) ? SLEEP_MODE_AWAKE : (chance(50) ? SLEEP_MODE_PRESLEEP : SLEEP_MODE_SLEEP);
        if (chance(1)) w->blocked = chance(12);
        if (chance(1)) w->data_valid = chance(92);
        if (chance(1)) w->evaluated = chance(94);
        if (chance(1)) new_gains(w);
        if (chance(1)) w->chaos = chance(15);
        if (chance(1)) w->plant = chance(60) ? 20.0 + frand() * 450.0 : to_fixed(0) * 1.0;   /* внезапный скачок температуры */
        if (chance(1)) w->sample_period = 11 + rnd() % 80U;

        /* новый отсчёт АЦП */
        if ((int32_t)(g_tick - w->next_sample) >= 0) {
            w->update_tick = chance(2) ? w->update_tick : (chance(4) ? w->update_tick + 1U : g_tick);   /* иногда тик не меняется, иногда +1 мс ровно */
            double noise = (frand() - 0.5) * 1.0;
            w->meas_temp = w->chaos ? to_fixed(-60.0 + frand() * 700.0) : to_fixed(w->plant + noise);
            if (chance(6)) {
                /* ровно на границах: уставка, уставка +/- полоса интеграла, +/- 1 LSB — проверка строгости >, >=, <, <= */
                static const int32_t k_edge[] = { 0, 1, -1, 8 * 65536, 8 * 65536 + 1, 8 * 65536 - 1, -8 * 65536, -8 * 65536 + 1, -8 * 65536 - 1,
                                                  15 * 65536, -15 * 65536, 30 * 65536 };
                w->meas_temp = w->setpoint - k_edge[rnd() % (sizeof k_edge / sizeof k_edge[0])];
            }
            w->next_sample = g_tick + w->sample_period + (chance(10) ? rnd() % 40U : 0U);
        }
    }
}

static void observe(void)
{
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        ev("state", "%d:duty=%u,int=%d,dTdt=%d,dv=%d,prev=%d,pv=%d,last=%u,smooth=%d", c,
           s_duty_pct[c], (int)s_integral[c], (int)s_dTdt[c], (int)s_dTdt_valid[c], (int)s_prev_temp[c],
           (int)s_prev_temp_valid[c], (unsigned)s_last_update_tick[c], (int)Control_GetSmoothedPowerPct((channel_id_t)c));
    }
    ev("presleep", "%d", (int)Control_PresleepSetpoint(CHANNEL_SOLDER, W[0].setpoint));
}

int main(int argc, char **argv)
{
    long steps = (argc > 1) ? atol(argv[1]) : 200000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    if (getenv("DUMP")) g_dump_step = atol(getenv("DUMP"));
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();
    g_tick = chance(30) ? 0xFFFFFFFFU - rnd() % 20000U : rnd() % 100000U;

    world_init();
    g_step = -1;
    Control_Init();
    observe();

    for (g_step = 0; g_step < steps; g_step++) {
        uint32_t dt = chance(85) ? CONTROL_POLL_MS : (chance(70) ? rnd() % 40U : 100U + rnd() % 3000U);
        g_tick += dt;
        world_step(dt);
        if (chance(1) && g_step % 500 == 0) { Control_Init(); ev("init", ""); }
        Control_Poll();
        observe();
    }
    printf("RESULT steps=%ld seed=%llu hash=%016llx events=%llu\n", steps, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
