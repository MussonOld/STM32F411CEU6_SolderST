/*
 * Golden-master harness для App/Buttons/buttons.c (хост, gcc; в прошивку не входит).
 *
 * Дополняет tests/buttons/test_buttons.c (сценарные проверки): здесь на вход идёт
 * случайный поток состояний GPIOA->IDR (одиночные кнопки, аккорды UP+DN, лишние
 * пары/тройки, дребезг, "передача с руки на руку") и тиков, а на выход — вся
 * наблюдаемая трасса: события из очереди (тип + маска), Buttons_IsHeld() всех
 * кнопок, Buttons_IsIgnored(). Время "прицеливается" в пороги относительно начала
 * эпизода (окно аккорда, long-press) с шагом в 1 мс — границы >= / > проверяются
 * точно. Очередь иногда не вычитывается — проверяется переполнение (8 событий).
 * Рефакторинг buttons.c обязан сохранить трассу побайтно на том же seed.
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

#include "buttons.h"
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
GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc;
uint32_t HAL_GetTick(void) { return g_tick; }

static const uint16_t k_pins[BUTTON_COUNT] = {
    [BUTTON_SET1] = SET1_Pin, [BUTTON_SET2] = SET2_Pin, [BUTTON_SET3] = SET3_Pin,
    [BUTTON_DN] = DN_Pin, [BUTTON_UP] = UP_Pin, [BUTTON_TOOLS] = TOOLS_Pin,
};

static uint8_t g_target;     /* какие кнопки "хотят" быть нажаты (маска button_id_t) */
static long    g_hold_left;  /* сколько опросов держим текущий target */
static uint32_t g_ep_tick;   /* тик опроса, на котором впервые подтвердилась нажатая кнопка (начало эпизода) */
static bool    g_ep_valid;

static void drive_gpio(uint8_t pressed_mask)
{
    uint16_t idr = 0xFFFFU;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (pressed_mask & BUTTON_MASK(i)) idr = (uint16_t)(idr & ~k_pins[i]);
    }
    test_gpioa.IDR = idr;
}

static uint8_t random_target(void)
{
    unsigned r = rnd() % 100U;
    if (r < 22) return 0;
    if (r < 62) return BUTTON_MASK(rnd() % BUTTON_COUNT);                  /* одиночная */
    if (r < 76) return BUTTONS_CHORD_UP_DN_MASK;                           /* аккорд UP+DN */
    if (r < 84) return (uint8_t)(BUTTON_MASK(rnd() % BUTTON_COUNT) | BUTTON_MASK(rnd() % BUTTON_COUNT)); /* пара (в т.ч. не-аккорд) */
    if (r < 90) return (uint8_t)(rnd() & 0x3FU);                           /* произвольная */
    if (r < 95) return BUTTON_MASK(BUTTON_UP);
    return BUTTON_MASK(BUTTON_DN);
}

static void drain(const char *tag)
{
    button_event_t e;
    int n = 0;
    while (Buttons_PopEvent(&e)) {
        ev(tag, "%d,%02x", (int)e.type, e.mask);
        if (++n > 64) { ev("DRAIN_RUNAWAY", ""); break; }
    }
}

static void observe(void)
{
    uint8_t held = 0;
    for (int i = 0; i < BUTTON_COUNT + 1; i++) { /* +1: заведомо неверный id */
        if (Buttons_IsHeld((button_id_t)i)) held |= (uint8_t)(1u << i);
    }
    ev("state", "held=%02x,ignored=%d", held, (int)Buttons_IsIgnored());
}

/** Переполнение очереди: 12 коротких нажатий подряд без единого вычитывания (ёмкость очереди — 8), затем вычитываем. */
static void queue_overflow_scenario(void)
{
    ev("overflow", "");
    for (int k = 0; k < 12; k++) {
        drive_gpio(BUTTON_MASK(BUTTON_SET1));
        for (int i = 0; i < 6; i++) { g_tick += 10U; Buttons_Poll(); }
        drive_gpio(0);
        for (int i = 0; i < 6; i++) { g_tick += 10U; Buttons_Poll(); }
    }
    g_ep_valid = false;
    observe();
    drain("overflow_event");
}

int main(int argc, char **argv)
{
    long steps = (argc > 1) ? atol(argv[1]) : 300000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    if (getenv("DUMP")) g_dump_step = atol(getenv("DUMP"));
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();

    g_tick = chance(25) ? 0xFFFFFFFFU - (rnd() % 5000U) : rnd() % 100000U; /* иногда — у переполнения */
    g_step = -1;
    drive_gpio(0);
    Buttons_Init();
    observe();

    for (g_step = 0; g_step < steps; g_step++) {
        /* --- время --- */
        uint32_t dt;
        unsigned r = rnd() % 100U;
        if (g_ep_valid && r < 30) {
            /* прицел: порог окна аккорда или long-press относительно начала эпизода, ±2 мс */
            uint32_t base = chance(50) ? BUTTONS_CHORD_WINDOW_MS : BUTTONS_LONG_PRESS_MS;
            uint32_t want = g_ep_tick + base - 2U + rnd() % 5U;
            dt = (want - g_tick < 0x80000000U) ? (want - g_tick) : 1U; /* только вперёд */
        } else if (r < 70) dt = 10U;
        else if (r < 85) dt = rnd() % 5U;
        else if (r < 95) dt = 20U + rnd() % 300U;
        else dt = 400U + rnd() % 1500U;
        g_tick += dt;

        /* --- вход --- */
        if (g_hold_left <= 0) { g_target = random_target(); g_hold_left = 1 + (long)(rnd() % 14U); }
        g_hold_left--;
        uint8_t raw = g_target;
        if (chance(8)) raw ^= BUTTON_MASK(rnd() % BUTTON_COUNT); /* дребезг: одна кнопка на один опрос иначе */
        if (chance(2)) { g_target = random_target(); }            /* резкая смена посреди удержания: "передача с руки на руку" */
        drive_gpio(raw);

        if (chance(1)) { Buttons_Init(); ev("init", ""); g_ep_valid = false; }
        if (chance(1)) { queue_overflow_scenario(); g_hold_left = 0; }

        bool held_before = false;
        for (int i = 0; i < BUTTON_COUNT; i++) if (Buttons_IsHeld((button_id_t)i)) held_before = true;
        ev("poll", "%u,%02x", (unsigned)g_tick, raw);
        Buttons_Poll();
        bool held_after = false;
        for (int i = 0; i < BUTTON_COUNT; i++) if (Buttons_IsHeld((button_id_t)i)) held_after = true;
        if (!held_before && held_after) { g_ep_tick = g_tick; g_ep_valid = true; }
        if (!held_after) g_ep_valid = false;

        observe();
        if (chance(75)) drain("event");   /* иногда не вычитываем — очередь (8) переполняется */
        if (chance(1)) { ev("pop_null", "%d", (int)Buttons_PopEvent(NULL)); }
    }
    drain("final");
    printf("RESULT steps=%ld seed=%llu hash=%016llx events=%llu\n", steps, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
