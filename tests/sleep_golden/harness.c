/*
 * Golden-master harness для App/Sleep/sleep.c (хост, gcc; в прошивку не входит).
 *
 * Sleep — таймерная FSM на канал: на вход сырые уровни Dock/Btn_Pump, "инструмент
 * подключен", "канал включён", таймауты из Settings, HAL_GetTick() (в т.ч. с
 * переполнением и значением 0), Sleep_ForceAwake(); на выход — Sleep_GetMode(),
 * Sleep_GetRemainingSeconds() и сигнал Beep_EnteredSleep(). Всё окружение подменено
 * моделью; трасса (наблюдаемое состояние на каждом шаге + каждый сигнал) хешируется.
 * Рефакторинг sleep.c обязан сохранить трассу побайтно на том же seed.
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

#include "sleep.h"
#include "settings.h"
#include "error.h"
#include "state.h"
#include "beep.h"
#include "main.h"
#include "stm32f4xx_hal.h"

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

/* ------------------------------------------------------------ модель мира */
static uint32_t g_tick;
static bool g_dock = true, g_pump = true;       /* сырые уровни: 1 = "простой" у обоих входов */
static bool g_absent[CHANNEL_COUNT];           /* Error_IsChannelIdle(): инструмент не подключен */
static bool g_enabled[CHANNEL_COUNT];
static uint16_t g_presleep[CHANNEL_COUNT], g_sleep[CHANNEL_COUNT]; /* минуты */

GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc;
uint32_t HAL_GetTick(void) { return g_tick; }
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    if (port == Dock_GPIO_Port && pin == Dock_Pin)         return g_dock ? GPIO_PIN_SET : GPIO_PIN_RESET;
    if (port == Btn_Pump_GPIO_Port && pin == Btn_Pump_Pin) return g_pump ? GPIO_PIN_SET : GPIO_PIN_RESET;
    ev("BAD_PIN", "%p,%u", (void *)port, pin);
    return GPIO_PIN_RESET;
}
bool Error_IsChannelIdle(channel_id_t ch) { return g_absent[ch]; }
bool State_IsEnabled(channel_id_t ch) { return g_enabled[ch]; }
uint16_t Settings_GetPreSleepTimeout(channel_id_t ch) { return g_presleep[ch]; }
uint16_t Settings_GetSleepTimeout(channel_id_t ch) { return g_sleep[ch]; }
void Beep_EnteredSleep(void) { ev("BEEP", "t=%u", (unsigned)g_tick); }

static void observe(void)
{
    for (int c = 0; c < CHANNEL_COUNT + 1; c++) { /* +1: заведомо неверный канал */
        ev("ch", "%d,%d,%u", c, (int)Sleep_GetMode((channel_id_t)c), (unsigned)Sleep_GetRemainingSeconds((channel_id_t)c));
    }
}

static uint32_t pick_dt(void)
{
    unsigned r = rnd() % 100U;
    if (r < 55) return 10U;                          /* штатный период опроса */
    if (r < 75) return rnd() % 25U;                  /* мелкие шаги, в т.ч. 0 */
    if (r < 90) return 500U + rnd() % 3000U;         /* секунды */
    if (r < 97) return 20000U + rnd() % 200000U;     /* минуты */
    return 600000U + rnd() % 4000000U;               /* десятки минут */
}

/** Шаг времени: обычный случайный, либо "прицельный" — к ближайшему порогу таймера (последняя секунда
 *  до срабатывания, затем шаги по 1 мс), чтобы проверялись границы ">=" / ">" ровно в миллисекунду. */
static uint32_t aimed_dt(void)
{
    uint32_t dt = pick_dt();
    bool in_last_second = false;
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        if (Sleep_GetRemainingSeconds((channel_id_t)c) == 1U) in_last_second = true;
    }
    if (in_last_second && chance(85)) {
        return chance(80) ? 1U : 0U;
    }
    if (chance(25)) {
        channel_id_t c = (channel_id_t)(rnd() % CHANNEL_COUNT);
        uint32_t r = Sleep_GetRemainingSeconds(c);
        if (r >= 2U) dt = (r - 1U) * 1000U; /* остаток окажется в пределах последней секунды */
    }
    return dt;
}

/** Фронт "занят -> простаивает" ровно на HAL_GetTick()==0 (после переполнения): особый случай idle_start_tick. */
static void zero_edge_scenario(void)
{
    ev("zero_edge", "");
    for (int c = 0; c < CHANNEL_COUNT; c++) g_absent[c] = false;
    g_dock = false; g_pump = false;
    g_tick = 0xFFFFFFFFU - 60U;
    for (int i = 0; i < 4; i++) { g_tick += 10U; Sleep_Poll(); }
    g_dock = true; g_pump = true;
    g_tick = 0xFFFFFFFEU; Sleep_Poll();
    g_tick = 0xFFFFFFFFU; Sleep_Poll();
    g_tick = 0U;          Sleep_Poll(); /* третий стабильный опрос — фронт на тике 0 */
}

int main(int argc, char **argv)
{
    long steps = (argc > 1) ? atol(argv[1]) : 300000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    if (getenv("DUMP")) g_dump_step = atol(getenv("DUMP"));
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();

    for (int c = 0; c < CHANNEL_COUNT; c++) { g_enabled[c] = true; g_presleep[c] = 1; g_sleep[c] = 2; }
    /* стартовое время: 0, 1, у переполнения или случайное */
    switch (rnd() % 4U) {
        case 0: g_tick = 0; break;
        case 1: g_tick = 1; break;
        case 2: g_tick = 0xFFFFFFFFU - (rnd() % 120000U); break;
        default: g_tick = rnd(); break;
    }
    g_step = -1;
    Sleep_Init();
    observe();

    for (g_step = 0; g_step < steps; g_step++) {
        /* --- мир меняется --- */
        g_tick += aimed_dt();
        if (chance(1)) g_tick = 0xFFFFFFFFU - (rnd() % 70000U);    /* скачок к переполнению: ближайшие шаги пересекают 0 */
        if (chance(3)) { g_dock = chance(50); }
        if (chance(3)) { g_pump = chance(50); }
        if (chance(10)) { /* дребезг: на пару опросов вход мигает */
            g_dock = !g_dock; g_pump = !g_pump;
        }
        for (int c = 0; c < CHANNEL_COUNT; c++) {
            if (chance(2)) g_absent[c] = chance(25);
            if (chance(2)) g_enabled[c] = chance(80);
            if (chance(2)) g_presleep[c] = chance(25) ? 0 : (uint16_t)(1 + rnd() % 4U);
            if (chance(2)) g_sleep[c]    = chance(25) ? 0 : (uint16_t)(1 + rnd() % 4U);
        }
        /* --- вызовы модуля --- */
        if (chance(1)) { Sleep_Init(); ev("init", ""); }
        if (chance(1)) zero_edge_scenario();
        if (chance(2)) {
            channel_id_t c = (channel_id_t)(rnd() % (CHANNEL_COUNT + 1U)); /* иногда неверный канал */
            Sleep_ForceAwake(c);
            ev("force", "%d", (int)c);
        }
        ev("poll", "%u,%d%d,%d%d,%d%d,%u/%u,%u/%u", (unsigned)g_tick, g_dock, g_pump, g_absent[0], g_absent[1],
           g_enabled[0], g_enabled[1], g_presleep[0], g_sleep[0], g_presleep[1], g_sleep[1]);
        Sleep_Poll();
        if (chance(30)) Sleep_Poll(); /* два опроса подряд на том же тике — дебаунс считает опросы, не время */
        observe();
    }
    printf("RESULT steps=%ld seed=%llu hash=%016llx events=%llu\n", steps, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
