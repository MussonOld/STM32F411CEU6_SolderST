/*
 * Golden-master harness для App/Screen/screen*.c (хост, gcc; в прошивку не входит).
 *
 * Идея: рефакторинг экрана не должен менять НИ ОДНОГО обращения к функциям Display_ и TextField_.
 * Все внешние зависимости Screen подменены заглушками, которые пишут каждое
 * рисующее обращение (имя + аргументы + содержимое пикселей) в хеш-трассу.
 * Геттеры состояния (State/Error/Sleep/Settings/Menu/FSM) — чистые функции
 * от "мира", который драйвер случайно (но детерминированно, seed) меняет от кадра
 * к кадру. Две сборки (старый и новый screen*.c) на одном seed обязаны дать
 * одинаковые трассы.
 *
 *   ./golden [frames] [seed]        -> печатает итоговый хеш и число событий
 *   TRACE=1 ./golden ...            -> по строке "frame N hash events" на кадр
 *   DUMP=N  ./golden ...            -> текстовый дамп событий кадра N
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "screen.h"
#include "channel.h"
#include "fonts.h"
#include "display.h"
#include "fixed_point.h"
#include "sleep.h"
#include "fsm.h"
#include "settings.h"
#include "text_field.h"
#include "ads1220.h"
#include "screen_layout.h" /* LINE_INFO_SLEEP_*: инвариант "у заблокированного канала нет таймера сна" */

/* ------------------------------------------------------------------ трасса */
static uint64_t g_hash = 1469598103934665603ULL;
static uint64_t g_events;
static long g_frame;
static long g_dump_frame = -2;
static bool g_nolive;  /* NOLIVE=1: меню никогда не просит живую температуру + строка живой температуры не попадает в трассу — режим сверки со старым экраном */
static bool g_nofault; /* NOFAULT=1: аварий/блокировок нет вовсе (ГСЧ тот же) — режим сверки со старым поведением */

static void mix_u64(uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        g_hash ^= (uint8_t)(v >> (8 * i));
        g_hash *= 1099511628211ULL;
    }
}
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
    if (g_frame == g_dump_frame) printf("  %s(%s)\n", name, buf);
}

/* ------------------------------------------------------------------- мир */
static uint64_t g_rng = 88172645463325252ULL;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
static bool chance(unsigned pct) { return (rnd() % 100U) < pct; }

static struct {
    channel_id_t active;
    screen_mode_t mode;
    bool enabled[CHANNEL_COUNT], idle[CHANNEL_COUNT], faulted[CHANNEL_COUNT], blocked[CHANNEL_COUNT];
    int fault_msg[CHANNEL_COUNT];
    sleep_mode_t sleep[CHANNEL_COUNT];
    uint32_t remaining[CHANNEL_COUNT];
    fixed_t temp[CHANNEL_COUNT];
    fixed_t power[CHANNEL_COUNT];
    uint16_t target[CHANNEL_COUNT];
    uint16_t preset[CHANNEL_COUNT][PRESET_COUNT];
    uint16_t presleep_timeout[CHANNEL_COUNT];
    uint8_t splash_mode;
    bool pending;
    int info_msg;
    uint8_t cursor, item_count;
    bool editing, expert_warn, reset_confirm, reset_done;
    uint32_t settled_mask;
    bool dma_fail, win_fail;
    bool live;                       /* Menu_ShowsLiveTemp() */
    bool ads_valid[CHANNEL_COUNT];
    fixed_t ads_temp[CHANNEL_COUNT];
    uint16_t width_seed;
    uint32_t tick;   /* HAL_GetTick() — фаза мигания остывающей температуры */
} W;

static const char *const k_fault_msgs[] = { NULL, NULL, NULL, "Обрыв нагревателя", "КЗ RTD", "ERR ADS1220", "Однослово" };
static const char *const k_info_msgs[]  = { NULL, "EEPROM: ошибка", "Сбой БП", "Станция готова", "" };
static const uint32_t k_remaining[] = { 0, 1, 9, 10, 59, 60, 61, 119, 120, 599, 600, 3599, 3600, 5999, 6000, 36000 };

static void world_init(void)
{
    memset(&W, 0, sizeof W);
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        W.enabled[c] = true; W.target[c] = 300; W.temp[c] = FIXED_FROM_INT(25);
        W.presleep_timeout[c] = 60;
    }
    W.item_count = 8; W.splash_mode = 1; W.settled_mask = ~0u;
}

static void world_step(void)
{
    if (chance(3)) W.active = (channel_id_t)(rnd() % CHANNEL_COUNT);
    if (chance(2)) W.mode = chance(30) ? SCREEN_MODE_SERVICE : SCREEN_MODE_MAIN;
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        if (chance(4)) W.enabled[c] = chance(85);
        if (chance(5)) W.idle[c] = chance(25);
        if (chance(4)) { W.faulted[c] = chance(20); W.fault_msg[c] = W.faulted[c] ? 3 + (int)(rnd() % 4U) : 0; }
        if (chance(4)) W.blocked[c] = chance(20);
        if (chance(8)) W.sleep[c] = (sleep_mode_t)(rnd() % 3U);
        if (chance(30)) W.remaining[c] = k_remaining[rnd() % (sizeof k_remaining / sizeof k_remaining[0])];
        if (chance(35)) {
            /* температура: целая часть + дробь, в т.ч. у границ гистерезиса (0.3), отрицательные и >999 */
            int32_t base = (int32_t)(rnd() % 520U) - 20;
            if (chance(50) && W.temp[c] != 0) base = FIXED_TO_INT(W.temp[c]) + (int32_t)(rnd() % 3U) - 1;
            fixed_t frac = chance(40) ? (fixed_t)(FIXED_ONE * 3 / 10 + (int32_t)(rnd() % 5U) - 2)
                                      : (fixed_t)(rnd() % (uint32_t)FIXED_ONE);
            if (chance(15)) frac = (fixed_t)(FIXED_ONE - 1 - (int32_t)(rnd() % 3U));
            W.temp[c] = FIXED_FROM_INT(base) + frac;
        }
        if (chance(20)) W.power[c] = (fixed_t)((rnd() % 101U) << FIXED_SHIFT) + (chance(50) ? (fixed_t)(rnd() % (uint32_t)FIXED_ONE) : 0);
        if (chance(5)) W.target[c] = (uint16_t)(rnd() % 500U);
        if (chance(3)) for (int p = 0; p < PRESET_COUNT; p++) W.preset[c][p] = (uint16_t)(rnd() % 500U);
        if (chance(3)) W.presleep_timeout[c] = chance(25) ? 0 : (uint16_t)(rnd() % 600U);
    }
    W.tick += 1U + (rnd() % 700U);
    if (chance(2)) W.splash_mode = (uint8_t)(rnd() % 3U);
    if (chance(8)) W.pending = chance(40);
    if (chance(4)) W.info_msg = (int)(rnd() % (sizeof k_info_msgs / sizeof k_info_msgs[0]));
    if (chance(10)) W.cursor = (uint8_t)(rnd() % 8U);
    if (chance(3)) W.item_count = chance(50) ? 8 : 7;
    if (chance(6)) W.editing = chance(30);
    if (chance(3)) { W.expert_warn = chance(20); W.reset_confirm = !W.expert_warn && chance(30); W.reset_done = !W.expert_warn && !W.reset_confirm && chance(40); }
    if (g_nofault) {
        for (int c = 0; c < CHANNEL_COUNT; c++) { W.faulted[c] = false; W.fault_msg[c] = 0; W.blocked[c] = false; }
    }
    W.settled_mask = chance(70) ? ~0u : rnd();
    W.dma_fail = chance(3);
    W.win_fail = chance(3);
    if (chance(5)) W.width_seed = (uint16_t)rnd();
    if (chance(6)) W.live = chance(45);
    for (int c = 0; c < CHANNEL_COUNT; c++) {
        if (chance(5)) W.ads_valid[c] = chance(85);
        if (chance(40)) W.ads_temp[c] = (fixed_t)((int32_t)(rnd() % (650U << 16)) - (50 << 16)) + (fixed_t)(chance(40) ? (FIXED_ONE * 3 / 10 + (int32_t)(rnd() % 5U) - 2) : 0);
    }
}

/* ------------------------------------------------- заглушки: чистые геттеры */
fixed_t Control_GetSmoothedPowerPct(channel_id_t ch) { return W.power[ch]; }
fixed_t Control_PresleepSetpoint(channel_id_t ch, fixed_t sp) { return sp > FIXED_FROM_INT(120 + 5 * (int)ch) ? FIXED_FROM_INT(120 + 5 * (int)ch) : sp; }
const char *Error_GetChannelFaultMessage(channel_id_t ch) { return k_fault_msgs[W.fault_msg[ch]]; }
const char *Error_GetInfoZoneMessage(void) { return k_info_msgs[W.info_msg]; }
/* как в error.c: БП (W.blocked — глобальный флаг канала в этой модели) либо любой tool_fault != NONE (авария ИЛИ "не подключен") */
bool Error_IsChannelBlocked(channel_id_t ch) { return W.blocked[ch] || W.faulted[ch] || (W.idle[ch] && !W.faulted[ch]); }
bool Error_IsChannelFaulted(channel_id_t ch) { return W.faulted[ch]; }
bool Error_IsChannelIdle(channel_id_t ch) { return W.idle[ch] && !W.faulted[ch]; /* idle и fault не пересекаются, см. error.h */ }
channel_id_t InputFSM_GetActiveChannel(void) { return W.active; }
screen_mode_t InputFSM_GetScreenMode(void) { return W.mode; }
uint8_t Menu_GetCursor(void) { return W.cursor % W.item_count; }
uint8_t Menu_GetItemCount(void) { return W.item_count; }
bool Menu_IsEditing(void) { return W.editing; }
bool Menu_ShowsLiveTemp(void) { return !g_nolive && W.live && !W.expert_warn && !W.reset_confirm && !W.reset_done; }
bool ADS1220_IsDataValid(channel_id_t ch) { return W.ads_valid[ch]; }
fixed_t ADS1220_GetTemperatureC(channel_id_t ch) { return W.ads_temp[ch]; }
#define HARNESS_MENU_TEMP_LINE ((uint8_t)(LINE_MENU_ITEM_7 + 1)) /* LINE_MENU_TEMP (в старом экране такой строки нет) */
bool Menu_IsShowingExpertWarning(void) { return W.expert_warn; }
bool Menu_IsShowingResetConfirm(void) { return W.reset_confirm; }
bool Menu_IsShowingResetDone(void) { return W.reset_done; }
const char *Menu_GetTitle(void) { return W.active == CHANNEL_SOLDER ? "Настройка Паяльник" : "Настройка Отсос"; }
const char *Menu_GetItemLabel(uint8_t i) { static const char *const l[] = {"Bzzz","Presleep time","Presleep temp","Standby","Заставка","Сброс","Expert","Ещё"}; return l[i % 8U]; }
void Menu_GetItemValueText(uint8_t i, char *buf, uint8_t n)
{
    unsigned v = (i * 37U + W.width_seed) % 1000U;
    if (v % 5U == 0U) buf[0] = '\0'; /* пункты без значения (Сброс/Expert) */
    else snprintf(buf, n, "%u", v);
}
const char *Menu_GetExpertWarningLine(uint8_t i) { static const char *const l[] = {"Внимание!", "Режим Expert", "Продолжить?"}; return l[i % 3U]; }
const char *Menu_GetResetConfirmLine(uint8_t i) { static const char *const l[] = {"Сбросить?", "Да / Нет", "UP / DN"}; return l[i % 3U]; }
const char *Menu_GetResetDoneLine(uint8_t i) { static const char *const l[] = {"Сброс выполнен", "", ""}; return l[i % 3U]; }
uint16_t Settings_GetPreSleepTimeout(channel_id_t ch) { return W.presleep_timeout[ch]; }
uint16_t Settings_GetPreset(channel_id_t ch, preset_id_t p) { return W.preset[ch][p]; }
uint8_t Settings_GetSplashMode(void) { return W.splash_mode; }
uint16_t Settings_GetTarget(channel_id_t ch) { return W.target[ch]; }
uint32_t HAL_GetTick(void) { return W.tick; }
bool Settings_HasPendingChanges(void) { return W.pending; }
sleep_mode_t Sleep_GetMode(channel_id_t ch) { return W.sleep[ch]; }
uint32_t Sleep_GetRemainingSeconds(channel_id_t ch) { return W.remaining[ch]; }
fixed_t State_GetCurrentTemp(channel_id_t ch) { return W.temp[ch]; }
bool State_IsEnabled(channel_id_t ch) { return W.enabled[ch]; }
bool TextField_IsSettled(uint8_t line) { return (W.settled_mask >> (line & 31U)) & 1U; }
uint16_t TextField_GetShownWidth(uint8_t line) { return (uint16_t)(((line * 13U) ^ W.width_seed) % 70U); }

/* ------------------------------------------------ заглушки: рисующие вызовы */
static int font_id(const font_t *f)
{
    if (f == &AntiquaB_18_uni) return 18;
    if (f == &AntiquaB_24_uni) return 24;
    if (f == &AntiquaB_32_uni) return 32;
    if (f == &Comic_60_dig)    return 60;
    return -1;
}
void TextField_ConfigureLine(uint8_t line, uint16_t x, uint16_t y, const font_t *font, display_color_t fg, display_color_t bg)
{ if (g_nolive && line == HARNESS_MENU_TEMP_LINE) return; ev("Cfg", "%u,%u,%u,f%d,%04x,%04x", line, x, y, font_id(font), fg, bg); }
void TextField_SetColors(uint8_t line, display_color_t fg, display_color_t bg)
{ ev("Col", "%u,%04x,%04x", line, fg, bg); }
void TextField_InvalidateAll(void) { ev("InvAll", ""); }

#define FMT_BODY(out) char out[256]; { va_list ap; va_start(ap, fmt); vsnprintf(out, sizeof out, fmt, ap); va_end(ap); }
void TextField_Printf(uint8_t line, const char *fmt, ...)
{ FMT_BODY(s) ev("Print", "%u,'%s'", line, s); }
void TextField_PrintfCentered(uint8_t line, uint16_t cx, const char *fmt, ...)
{
    FMT_BODY(s)
    if (line == HARNESS_MENU_TEMP_LINE) {
        /* Инвариант: живая температура в меню — только в режиме меню, на калибровочном пункте, при исправном канале и валидном АЦП */
        if (s[0] != '\0' && !(W.mode == SCREEN_MODE_SERVICE && Menu_ShowsLiveTemp() && !Error_IsChannelBlocked(W.active) && W.ads_valid[W.active])) {
            printf("INVARIANT VIOLATED frame %ld: live temp '%s' shown out of place\n", g_frame, s);
            exit(2);
        }
        if (g_nolive) return;
    }
    ev("PrintC", "%u,%u,'%s'", line, cx, s);
}
void TextField_PrintfRightAligned(uint8_t line, uint16_t rx, const char *fmt, ...)
{
    FMT_BODY(s)
    ev("PrintR", "%u,%u,'%s'", line, rx, s);
    /* Инвариант: у заблокированного канала (не подключен / авария / БП) в инфозоне нет таймера сна. Нарушение = баг. */
    if ((line == LINE_INFO_SLEEP_SOLDER && Error_IsChannelBlocked(CHANNEL_SOLDER))
     || (line == LINE_INFO_SLEEP_DESOLDER && Error_IsChannelBlocked(CHANNEL_DESOLDER))) {
        if (s[0] != '\0') {
            printf("INVARIANT VIOLATED frame %ld: blocked channel shows sleep status '%s' on line %u\n", g_frame, s, line);
            exit(2);
        }
    }
}

bool Display_IsBusy(void) { return false; }
Display_Status_t Display_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{ ev("Win", "%u,%u,%u,%u", x0, y0, x1, y1); return W.win_fail ? DISPLAY_ERROR : DISPLAY_OK; }
Display_Status_t Display_FillColorDMA(display_color_t c, uint32_t n)
{ ev("Fill", "%04x,%u", c, n); return W.dma_fail ? DISPLAY_ERROR : DISPLAY_OK; }
Display_Status_t Display_WritePixelsDMA(const display_color_t *px, uint32_t n)
{
    ev("Pix", "%u", n);
    mix_bytes(px, (size_t)n * sizeof *px); /* содержимое растра тоже сверяем */
    return W.dma_fail ? DISPLAY_ERROR : DISPLAY_OK;
}

/* ----------------------------------------------------------------- драйвер */
int main(int argc, char **argv)
{
    long frames = (argc > 1) ? atol(argv[1]) : 20000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    bool trace = getenv("TRACE") != NULL;
    g_nofault = getenv("NOFAULT") != NULL;
    g_nolive = getenv("NOLIVE") != NULL;
    if (getenv("DUMP")) g_dump_frame = atol(getenv("DUMP"));
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();

    world_init();
    g_frame = -1;
    Screen_Init();
    if (trace) printf("init %016llx %llu\n", (unsigned long long)g_hash, (unsigned long long)g_events);

    for (g_frame = 0; g_frame < frames; g_frame++) {
        world_step();
        Screen_Update();
        for (int c = 0; c < CHANNEL_COUNT; c++) {
            int32_t t = -1;
            bool ok = Screen_GetShownTemp((channel_id_t)c, &t);
            mix_u64((uint64_t)ok); mix_u64((uint64_t)(int64_t)t);
        }
        if (trace) printf("frame %ld %016llx %llu\n", g_frame, (unsigned long long)g_hash, (unsigned long long)g_events);
    }
    printf("RESULT frames=%ld seed=%llu hash=%016llx events=%llu\n", frames, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
