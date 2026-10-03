/*
 * Golden-master harness для App/Settings/settings.c (хост, gcc; в прошивку не входит).
 *
 * Прогоняет Settings_Init()/Settings_Load()/Settings_Poll()/Settings_Save() и сеттеры на
 * случайном образе EEPROM-модели: пустой чип, корректный блок, корректный блок с полями
 * вне диапазона и ПЕРЕСЧИТАННОЙ контрольной суммой (случай "checksum сошёлся, а поля
 * мусор"), режим заставки 3, побитовые повреждения, неверный magic, случайный мусор;
 * плюс отказы EEPROM (одиночные и постоянные) на любом по счёту обращении. В хеш-трассу
 * идут: каждое обращение к EEPROM (адрес, длина, данные записи, статус), статус загрузки,
 * все значения Get*, признак отложенной записи, итоговый образ чипа. Время прицеливается
 * в пороги отложенной записи (1500 мс) и повтора (5000 мс) с шагом 1 мс.
 * Рефакторинг settings.c обязан сохранить трассу побайтно на том же seed.
 *
 *   ./golden [iterations] [seed]   -> RESULT-строка с итоговым хешем
 *   DUMP=N ./golden ...            -> текстовый дамп итерации N
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "settings.h"
#include "eeprom.h"
#include "stm32f4xx_hal.h"

static uint64_t g_hash = 1469598103934665603ULL;
static uint64_t g_events;
static long g_iter;
static long g_dump_iter = -2;

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
    if (g_iter == g_dump_iter) printf("  %s(%s)\n", name, buf);
}

static uint64_t g_rng = 88172645463325252ULL;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}
static bool chance(unsigned pct) { return (rnd() % 100U) < pct; }

/* ------------------------------------------------------------ модель EEPROM */
#define EE_SIZE 512U
static uint8_t g_ee[EE_SIZE];
static long g_fail_countdown = -1;  /* -1: отказов нет; иначе — через столько обращений откажет */
static bool g_fail_persist;
static bool g_failing;
static uint32_t g_tick;

uint32_t HAL_GetTick(void) { return g_tick; }
uint16_t EEPROM_GetSize(void) { return (uint16_t)EE_SIZE; }

static bool ee_call_fails(void)
{
    if (g_failing) return true;
    if (g_fail_countdown < 0) return false;
    if (g_fail_countdown == 0) {
        g_fail_countdown = -1;
        if (g_fail_persist) g_failing = true;
        return true;
    }
    g_fail_countdown--;
    return false;
}
EEPROM_Status_t EEPROM_WriteByte(uint16_t addr, uint8_t data)
{
    bool fail = ee_call_fails() || addr >= EE_SIZE;
    ev("WB", "%u,%02x,%d", addr, data, (int)fail);
    if (fail) return EEPROM_ERROR;
    g_ee[addr] = data;
    return EEPROM_OK;
}
EEPROM_Status_t EEPROM_ReadByte(uint16_t addr, uint8_t *data)
{
    bool fail = ee_call_fails() || addr >= EE_SIZE || data == NULL;
    ev("RB", "%u,%d", addr, (int)fail);
    if (fail) return EEPROM_ERROR;
    *data = g_ee[addr];
    return EEPROM_OK;
}
EEPROM_Status_t EEPROM_Write(uint16_t addr, const uint8_t *data, uint16_t len)
{
    bool fail = ee_call_fails() || (uint32_t)addr + len > EE_SIZE;
    ev("W", "%u,%u,%d", addr, len, (int)fail);
    if (fail) return EEPROM_ERROR;
    mix_bytes(data, len);
    memcpy(&g_ee[addr], data, len);
    return EEPROM_OK;
}
EEPROM_Status_t EEPROM_Read(uint16_t addr, uint8_t *data, uint16_t len)
{
    bool fail = ee_call_fails() || (uint32_t)addr + len > EE_SIZE;
    ev("R", "%u,%u,%d", addr, len, (int)fail);
    if (fail) return EEPROM_ERROR;
    memcpy(data, &g_ee[addr], len);
    return EEPROM_OK;
}

/* -------------------------------------------------- построение образа чипа */
static uint8_t crc8(const uint8_t *buf, unsigned len)
{
    uint8_t crc = 0;
    for (unsigned i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

typedef struct { uint16_t lo, hi; } range_t;
static const range_t k_ranges[12] = {
    {50,450},{50,450},{50,450},{50,450},{50,450},   /* preset x3, target, presleep_temp */
    {60,110},{190,250},                             /* slope, bias */
    {0,30},{0,30},                                  /* pre_sleep_timeout, sleep_timeout */
    {0,10000},{0,10000},{0,10000},                  /* kp, ki, kd */
};

static uint16_t random_field(range_t r)
{
    unsigned k = rnd() % 100U;
    if (k < 52) return (uint16_t)(r.lo + rnd() % (unsigned)(r.hi - r.lo + 1U));
    if (k < 64) return r.lo;
    if (k < 76) return r.hi;
    if (k < 84) return r.lo > 0 ? (uint16_t)(r.lo - 1U) : 0xFFFFU;
    if (k < 92) return (uint16_t)(r.hi + 1U);
    return (chance(50) ? 0xFFFFU : (uint16_t)rnd());
}

#define DATA_LEN 49U /* 2 канала x 24 байта + flags */
static void build_image(unsigned kind)
{
    memset(g_ee, 0xFF, sizeof g_ee);
    if (kind == 0) return; /* пустой чип */

    uint8_t data[DATA_LEN];
    bool in_range_only = (kind == 1);
    for (int ch = 0; ch < 2; ch++)
        for (int f = 0; f < 12; f++) {
            uint16_t v = in_range_only ? (uint16_t)(k_ranges[f].lo + rnd() % (unsigned)(k_ranges[f].hi - k_ranges[f].lo + 1U))
                                       : random_field(k_ranges[f]);
            data[ch * 24 + f * 2] = (uint8_t)(v & 0xFF);
            data[ch * 24 + f * 2 + 1] = (uint8_t)(v >> 8);
        }
    data[48] = in_range_only ? (uint8_t)((rnd() % 3U) << 1 | (rnd() & 1U))   /* заставка 0..2 */
                             : (chance(50) ? (uint8_t)rnd() : (uint8_t)((3U << 1) | (rnd() & 1U))); /* иногда режим 3 */
    g_ee[0] = 0x5A; g_ee[1] = 0xA5;               /* magic 0xA55A LE */
    g_ee[3] = 0;
    memcpy(&g_ee[4], data, DATA_LEN);
    g_ee[2] = crc8(data, DATA_LEN);               /* checksum сходится, даже когда поля мусор */

    if (kind == 4) {                              /* побитовое повреждение — checksum НЕ пересчитан */
        unsigned n = 1 + rnd() % 3U;
        for (unsigned i = 0; i < n; i++) g_ee[4 + rnd() % DATA_LEN] ^= (uint8_t)(1u << (rnd() % 8U));
    } else if (kind == 5) {                       /* случайный мусор целиком */
        for (unsigned i = 0; i < 64; i++) g_ee[i] = (uint8_t)rnd();
    } else if (kind == 6) {                       /* magic неверный (старая версия/чужой чип) */
        g_ee[0] ^= (uint8_t)(1u << (rnd() % 8U));
    }
}

static unsigned pick_kind(void)
{
    unsigned r = rnd() % 100U;
    if (r < 12) return 0;   /* пустой */
    if (r < 40) return 1;   /* валидный, всё в диапазоне */
    if (r < 70) return 2;   /* checksum сошёлся, поля вне диапазона / режим 3 */
    if (r < 80) return 3;
    if (r < 90) return 4;
    if (r < 95) return 5;
    return 6;
}

/* ----------------------------------------------------------------- наблюдение */
static void observe(const char *tag)
{
    for (int c = 0; c < CHANNEL_COUNT + 1; c++) { /* +1: неверный канал */
        channel_id_t ch = (channel_id_t)c;
        ev(tag, "ch%d:%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", c,
           Settings_GetPreset(ch, PRESET_1), Settings_GetPreset(ch, PRESET_2), Settings_GetPreset(ch, PRESET_3),
           Settings_GetTarget(ch), Settings_GetPresleepTemp(ch), Settings_GetSlope(ch), Settings_GetBias(ch),
           Settings_GetPreSleepTimeout(ch), Settings_GetSleepTimeout(ch), Settings_GetKp(ch), Settings_GetKi(ch), Settings_GetKd(ch));
    }
    ev("glob", "%02x,%u,%d", Settings_GetFlags(), Settings_GetSplashMode(), (int)Settings_HasPendingChanges());
    for (uint8_t b = 0; b < 9; b++) mix_bytes(&(const uint8_t){ (uint8_t)Settings_GetFlagBit(b) }, 1); /* бит 8 — вне байта */
}

static void random_setter(void)
{
    channel_id_t ch = chance(8) ? (channel_id_t)CHANNEL_COUNT : (channel_id_t)(rnd() % CHANNEL_COUNT);
    uint16_t v = chance(20) ? (uint16_t)rnd() : (uint16_t)(rnd() % 12000U);
    switch (rnd() % 14U) {
        case 0: Settings_SetPreset(ch, (preset_id_t)(chance(10) ? 7 : (int)(rnd() % PRESET_COUNT)), v); break;
        case 1: Settings_SetTarget(ch, v); break;
        case 2: Settings_SetPresleepTemp(ch, v); break;
        case 3: Settings_SetSlope(ch, v); break;
        case 4: Settings_SetBias(ch, v); break;
        case 5: Settings_SetPreSleepTimeout(ch, v); break;
        case 6: Settings_SetSleepTimeout(ch, v); break;
        case 7: Settings_SetKp(ch, v); break;
        case 8: Settings_SetKi(ch, v); break;
        case 9: Settings_SetKd(ch, v); break;
        case 10: Settings_SetFlags((uint8_t)rnd()); break;
        case 11: Settings_SetFlagBit((uint8_t)(rnd() % 10U), chance(50)); break;
        case 12: Settings_SetSplashMode((uint8_t)(rnd() % 5U)); break;
        default:
            if (chance(34)) Settings_ResetUserDefaults(ch);
            else if (chance(50)) Settings_ResetExpertDefaults(ch);
            else Settings_ResetGlobalUserDefaults();
            break;
    }
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? atol(argv[1]) : 40000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    if (getenv("DUMP")) g_dump_iter = atol(getenv("DUMP"));
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();
    g_tick = chance(30) ? 0xFFFFFFFFU - rnd() % 9000U : rnd() % 100000U;

    for (g_iter = 0; g_iter < iters; g_iter++) {
        build_image(pick_kind());
        g_failing = false; g_fail_countdown = -1; g_fail_persist = false;
        if (chance(30)) { g_fail_countdown = (long)(rnd() % 10U); g_fail_persist = chance(40); }

        Settings_Init();
        if (chance(20)) random_setter();
        g_tick += rnd() % 3U;
        SettingsLoadStatus_t st = Settings_Load();
        ev("load", "%d", (int)st);
        observe("after_load");

        /* отложенная запись и повтор: прицел в 1500 / 5000 мс относительно момента изменения */
        uint32_t base = g_tick;
        for (int k = 0; k < 6; k++) {
            unsigned r = rnd() % 100U;
            if (r < 20)      g_tick = base + 1499U;
            else if (r < 40) g_tick = base + 1500U;
            else if (r < 50) g_tick = base + 1501U;
            else if (r < 62) g_tick += 4999U;
            else if (r < 74) g_tick += 5000U;
            else if (r < 80) g_tick += 5001U;
            else             g_tick += rnd() % 700U;
            if (chance(25)) { random_setter(); base = g_tick; }
            if (chance(8) && g_failing) { g_failing = false; ev("bus_recovered", ""); }
            ev("poll", "%u", (unsigned)g_tick);
            Settings_Poll();
            observe("after_poll");
        }
        if (chance(10)) ev("save", "%d", (int)Settings_Save());
        mix_bytes(g_ee, sizeof g_ee);
    }
    printf("RESULT iters=%ld seed=%llu hash=%016llx events=%llu\n", iters, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
