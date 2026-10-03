/*
 * Golden-master harness для App/Menu/menu.c (хост, gcc; в прошивку не входит).
 *
 * Меню — конечный автомат: на вход события кнопок (Menu_HandleEvent), тики
 * (Menu_Poll), удержание кнопок (Buttons_IsHeld), активный канал; на выход —
 * возвращаемое действие, наблюдаемое состояние (курсор, режимы, подписи,
 * тексты значений) и все записи в Settings. Settings/FSM/кнопки подменены
 * моделью, каждая запись в Settings попадает в хеш-трассу вместе с аргументами.
 * Сторонний step_accel.c — настоящий. Рефакторинг меню обязан сохранить трассу
 * побайтно на том же seed.
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

#include "menu.h"
#include "settings.h"
#include "fsm.h"
#include "buttons.h"
#include "step_accel.h"
#include "stm32f4xx_hal.h"

static uint64_t g_hash = 1469598103934665603ULL;
static uint64_t g_events;
static bool g_equiv; /* EQUIV=1: имена Наклон/Смещение в трассе печатаются как Slope/Bias — сверка с эталоном до переименования */
static long g_step;
static long g_dump_step = -2;

static void mix_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { g_hash ^= b[i]; g_hash *= 1099511628211ULL; }
}
static void ev(const char *name, const char *fmt, ...)
{
    char buf[512];
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
static uint32_t g_reset_tick; /* HAL_GetTick() в момент последнего сброса — для проверки границы 3 с */
static bool     g_reset_seen;
static channel_id_t g_active;
static bool g_held[8];

/* модель Settings: значения по каналам; Set клампит к [0, max], как настоящие */
enum { P_PRESLEEP_TIME, P_PRESLEEP_TEMP, P_STANDBY, P_KP, P_KI, P_KD, P_SLOPE, P_BIAS, P_COUNT };
static const uint16_t k_pmax[P_COUNT] = { 600, 450, 600, 999, 999, 999, 400, 300 };
static const uint16_t k_pdef[P_COUNT] = { 60, 150, 120, 10, 5, 2, 89, 207 };
static uint16_t g_param[CHANNEL_COUNT][P_COUNT];
static uint8_t g_splash, g_flags;

static void world_init(void)
{
    for (int c = 0; c < CHANNEL_COUNT; c++)
        for (int p = 0; p < P_COUNT; p++) g_param[c][p] = k_pdef[p];
    g_splash = 1; g_flags = 0; g_tick = 1000; g_active = CHANNEL_SOLDER;
    memset(g_held, 0, sizeof g_held);
}

uint32_t HAL_GetTick(void) { return g_tick; }
channel_id_t InputFSM_GetActiveChannel(void) { return g_active; }
bool Buttons_IsHeld(button_id_t id) { return g_held[id]; }

static void set_param(const char *who, int p, channel_id_t ch, uint16_t v)
{
    ev(who, "ch%d,%u", (int)ch, (unsigned)v);
    g_param[ch][p] = v > k_pmax[p] ? k_pmax[p] : v;
}
void Settings_SetPreSleepTimeout(channel_id_t ch, uint16_t v) { set_param("SetPreSleep", P_PRESLEEP_TIME, ch, v); }
void Settings_SetPresleepTemp(channel_id_t ch, uint16_t v)    { set_param("SetPresleepTemp", P_PRESLEEP_TEMP, ch, v); }
void Settings_SetSleepTimeout(channel_id_t ch, uint16_t v)    { set_param("SetSleep", P_STANDBY, ch, v); }
void Settings_SetKp(channel_id_t ch, uint16_t v)    { set_param("SetKp", P_KP, ch, v); }
void Settings_SetKi(channel_id_t ch, uint16_t v)    { set_param("SetKi", P_KI, ch, v); }
void Settings_SetKd(channel_id_t ch, uint16_t v)    { set_param("SetKd", P_KD, ch, v); }
void Settings_SetSlope(channel_id_t ch, uint16_t v) { set_param("SetSlope", P_SLOPE, ch, v); }
void Settings_SetBias(channel_id_t ch, uint16_t v)  { set_param("SetBias", P_BIAS, ch, v); }
uint16_t Settings_GetPreSleepTimeout(channel_id_t ch) { return g_param[ch][P_PRESLEEP_TIME]; }
uint16_t Settings_GetPresleepTemp(channel_id_t ch)    { return g_param[ch][P_PRESLEEP_TEMP]; }
uint16_t Settings_GetSleepTimeout(channel_id_t ch)    { return g_param[ch][P_STANDBY]; }
uint16_t Settings_GetKp(channel_id_t ch)    { return g_param[ch][P_KP]; }
uint16_t Settings_GetKi(channel_id_t ch)    { return g_param[ch][P_KI]; }
uint16_t Settings_GetKd(channel_id_t ch)    { return g_param[ch][P_KD]; }
uint16_t Settings_GetSlope(channel_id_t ch) { return g_param[ch][P_SLOPE]; }
uint16_t Settings_GetBias(channel_id_t ch)  { return g_param[ch][P_BIAS]; }

void Settings_SetSplashMode(uint8_t mode) { ev("SetSplash", "%u", mode); g_splash = mode > 2 ? 2 : mode; }
uint8_t Settings_GetSplashMode(void) { return g_splash; }
void Settings_SetFlagBit(uint8_t bit, bool v) { ev("SetFlagBit", "%u,%d", bit, (int)v); if (v) g_flags |= (uint8_t)(1u << bit); else g_flags &= (uint8_t)~(1u << bit); }
bool Settings_GetFlagBit(uint8_t bit) { return (g_flags >> bit) & 1u; }

void Settings_ResetUserDefaults(channel_id_t ch)
{
    ev("ResetUser", "ch%d", (int)ch);
    g_reset_tick = g_tick; g_reset_seen = true;
    g_param[ch][P_PRESLEEP_TIME] = k_pdef[P_PRESLEEP_TIME];
    g_param[ch][P_PRESLEEP_TEMP] = k_pdef[P_PRESLEEP_TEMP];
    g_param[ch][P_STANDBY] = k_pdef[P_STANDBY];
}
void Settings_ResetGlobalUserDefaults(void) { ev("ResetGlobal", ""); g_flags = 0; g_splash = 1; }
void Settings_ResetExpertDefaults(channel_id_t ch)
{
    ev("ResetExpert", "ch%d", (int)ch);
    g_reset_tick = g_tick; g_reset_seen = true;
    for (int p = P_KP; p < P_COUNT; p++) g_param[ch][p] = k_pdef[p];
}

/* ----------------------------------------------------------- наблюдение */
static void observe(menu_action_t act)
{
    char buf[32];
    ev("action", "%d", (int)act);
    /* Инвариант (в хеш не входит): живая температура — ровно на Slope/Bias в списке/редактировании, не на экранах-сообщениях */
    {
        const char *lbl = Menu_GetItemLabel(Menu_GetCursor());
        bool expect = !Menu_IsShowingExpertWarning() && !Menu_IsShowingResetConfirm() && !Menu_IsShowingResetDone()
                      && (strcmp(lbl, "Kp") == 0 || strcmp(lbl, "Ki") == 0 || strcmp(lbl, "Kd") == 0
                          || strcmp(lbl, "Наклон") == 0 || strcmp(lbl, "Смещение") == 0);
        if (Menu_ShowsLiveTemp() != expect) {
            printf("INVARIANT VIOLATED step %ld: Menu_ShowsLiveTemp()=%d, expected %d (item '%s')\n", g_step, (int)Menu_ShowsLiveTemp(), (int)expect, lbl);
            exit(2);
        }
    }
    ev("cursor", "%u,%u", Menu_GetCursor(), Menu_GetItemCount());
    ev("flags", "%d%d%d%d", Menu_IsEditing(), Menu_IsShowingExpertWarning(), Menu_IsShowingResetConfirm(), Menu_IsShowingResetDone());
    ev("title", "%s", Menu_GetTitle());
    uint8_t n = Menu_GetItemCount();
    for (uint8_t i = 0; i < n + 1U; i++) { /* +1: индекс за концом списка */
        Menu_GetItemValueText(i, buf, sizeof buf);
        const char *label = Menu_GetItemLabel(i);
        if (g_equiv) {
            if (strcmp(label, "Наклон") == 0)   label = "Slope";
            if (strcmp(label, "Смещение") == 0) label = "Bias";
        }
        ev("item", "%u,'%s','%s'", i, label, buf);
    }
    for (uint8_t i = 0; i < 5; i++) {
        ev("lines", "%u,'%s','%s','%s'", i, Menu_GetExpertWarningLine(i), Menu_GetResetConfirmLine(i), Menu_GetResetDoneLine(i));
    }
    ev("model", "%u,%u,%u", g_splash, g_flags, g_param[g_active][P_KP]);
    for (int c = 0; c < CHANNEL_COUNT; c++) for (int p = 0; p < P_COUNT; p++) mix_bytes(&g_param[c][p], sizeof g_param[c][p]);
}

static button_event_t random_event(void)
{
    button_event_t e;
    unsigned r = rnd() % 100U;
    if (r < 36)      e.mask = BUTTON_MASK(BUTTON_UP);
    else if (r < 72) e.mask = BUTTON_MASK(BUTTON_DN);
    else if (r < 90) e.mask = BUTTON_MASK(BUTTON_SET2);
    else if (r < 94) e.mask = BUTTON_MASK(BUTTON_SET1);
    else if (r < 96) e.mask = BUTTON_MASK(BUTTON_SET3);
    else if (r < 98) e.mask = BUTTON_MASK(BUTTON_TOOLS);
    else             e.mask = BUTTONS_CHORD_UP_DN_MASK;
    unsigned t = rnd() % 100U;
    e.type = t < 52 ? BUTTON_EVENT_SHORT_PRESS : t < 90 ? BUTTON_EVENT_LONG_PRESS
           : t < 94 ? BUTTON_EVENT_CHORD_SHORT : t < 97 ? BUTTON_EVENT_CHORD_LONG : BUTTON_EVENT_VIOLATION;
    return e;
}

int main(int argc, char **argv)
{
    long steps = (argc > 1) ? atol(argv[1]) : 200000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    if (getenv("DUMP")) g_dump_step = atol(getenv("DUMP"));
    g_equiv = getenv("EQUIV") != NULL;
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();

    world_init();
    g_step = -1;
    Menu_Init();
    observe(MENU_ACTION_NONE);

    for (g_step = 0; g_step < steps; g_step++) {
        unsigned r = rnd() % 100U;
        if (r < 55) {
            button_event_t e = random_event();
            ev("event", "%02x,%d", e.mask, (int)e.type);
            observe(Menu_HandleEvent(&e));
        } else {
            /* тик: время идёт (иногда большими скачками — таймер "Сброс выполнен" 3 с), кнопки то держат, то отпускают */
            g_tick += chance(10) ? 3000U + rnd() % 500U : rnd() % 400U;
            if (g_reset_seen && chance(25)) {
                /* ровно у границы MENU_RESET_DONE_MS: -2..+2 мс от 3000 */
                uint32_t t = g_reset_tick + 2998U + rnd() % 5U;
                if (t > g_tick) g_tick = t;
            }
            if (chance(30)) { g_held[BUTTON_UP] = chance(60); g_held[BUTTON_DN] = chance(40); }
            if (chance(2)) g_active = (channel_id_t)(rnd() % CHANNEL_COUNT);
            if (chance(1)) { Menu_Init(); ev("init", ""); }
            ev("poll", "%u,%d%d", (unsigned)g_tick, g_held[BUTTON_UP], g_held[BUTTON_DN]);
            Menu_Poll();
            observe(MENU_ACTION_NONE);
        }
    }
    printf("RESULT steps=%ld seed=%llu hash=%016llx events=%llu\n", steps, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
