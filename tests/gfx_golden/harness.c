/*
 * Golden-master harness для App/UI/gfx.c (хост, gcc; в прошивку не входит).
 *
 * Прогоняет Gfx_DrawTextStart()/Gfx_Process()/Gfx_CancelJob()/Gfx_Measure* на настоящих
 * шрифтах проекта и на синтетических (пробел без битмапа, отрицательные xoffset/yoffset,
 * ячейка больше буфера глифа — ветка маркера переполнения), со случайными строками
 * (цифры, кириллица, символы вне шрифта, битый UTF-8, пустые), позициями у краёв экрана
 * (включая правее/левее экрана), прежним текстом для стирания и отказами дисплея:
 * Display_IsBusy(), Display_SetWindow() и Display_WritePixelsDMA() случайно отказывают.
 * Каждое обращение к Display_* (окно, число пикселей, СОДЕРЖИМОЕ пикселей) попадает в
 * хеш-трассу вместе со статусом задания после каждого Gfx_Process().
 * Собирается с -DNDEBUG (release-поведение: assert() отключён, работает маркер переполнения).
 * Рефакторинг gfx.c обязан сохранить трассу побайтно на том же seed.
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

#include "gfx.h"
#include "fonts.h"
#include "display.h"

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

/* ------------------------------------------------------------ модель дисплея */
static unsigned g_busy_pct, g_fail_window_pct, g_fail_dma_pct;

void Display_GetSize(uint16_t *w, uint16_t *h) { if (w) *w = 320; if (h) *h = 240; }
bool Display_IsBusy(void) { return chance(g_busy_pct); }
Display_Status_t Display_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    /* как настоящий драйвер: окно за краем экрана — ошибка геометрии */
    bool bad = x1 >= 320 || y1 >= 240 || x0 > x1 || y0 > y1;
    bool fail = bad || chance(g_fail_window_pct);
    ev("Win", "%u,%u,%u,%u,%d", x0, y0, x1, y1, (int)fail);
    return fail ? DISPLAY_ERROR : DISPLAY_OK;
}
Display_Status_t Display_WritePixelsDMA(const display_color_t *px, uint32_t n)
{
    bool fail = chance(g_fail_dma_pct);
    ev("Pix", "%u,%d", n, (int)fail);
    mix_bytes(px, (size_t)n * sizeof *px); /* содержимое растра тоже сверяем */
    return fail ? DISPLAY_ERROR : DISPLAY_OK;
}

/* ------------------------------------------------------------ синтетический шрифт */
/* глифы (lut по возрастанию кодов): ' ' ширина 0, advance 6 | A обычный | B xoff<0, yoff>0, overhang |
 * C yoff<0 | T ширина 0, advance 40 (ячейка ровно 2800 = размер буфера) | U ширина 40, advance 40 (ровно буфер) |
 * V ширина 60, advance 3 (ячейка больше буфера при малом advance -> маркер уже advance) |
 * W огромный advance (ячейка больше буфера) | X ширина 60, advance 0 (маркер нулевой ширины) |
 * Y ширина 0, advance 60 (пустая ячейка больше буфера) | Z ширина 0, advance 0 */
#define SYN_H 70U
#define SYN_N 11U
static const uint16_t syn_lut[SYN_N] = { ' ', 'A', 'B', 'C', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' };
static const uint8_t  syn_widths[SYN_N]  = { 0, 5, 7, 4, 0, 40, 60, 3, 60, 0, 0 };
static const int8_t   syn_xoff[SYN_N]    = { 0, 1, -3, 0, 0, 0, 0, 0, 0, 0, 0 };
static const int8_t   syn_yoff[SYN_N]    = { 0, 0, 3, -2, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t  syn_dwidth[SYN_N]  = { 6, 7, 5, 6, 40, 40, 3, 60, 0, 60, 0 };
static const uint8_t  syn_gh[SYN_N]      = { 0, SYN_H, SYN_H, SYN_H, 0, SYN_H, SYN_H, SYN_H, SYN_H, 0, 0 };
static uint16_t syn_offsets[SYN_N];
static uint8_t  syn_bitmap[8192];
static font_t   syn_font;

static void synthetic_font_init(void)
{
    unsigned bpc = (SYN_H + 7) / 8;
    unsigned off = 0;
    for (unsigned i = 0; i < SYN_N; i++) { syn_offsets[i] = (uint16_t)off; off += syn_widths[i] * bpc; }
    for (unsigned i = 0; i < sizeof syn_bitmap; i++) syn_bitmap[i] = (uint8_t)(rnd() & 0xFF);
    memset(&syn_font, 0, sizeof syn_font);
    syn_font.bitmap = syn_bitmap; syn_font.widths = syn_widths; syn_font.offsets = syn_offsets;
    syn_font.xoffset = syn_xoff; syn_font.yoffset = syn_yoff; syn_font.dwidth = syn_dwidth;
    syn_font.glyph_heights = syn_gh; syn_font.lut = syn_lut; syn_font.lut_size = SYN_N;
    syn_font.height = SYN_H; syn_font.first_char = ' '; syn_font.last_char = 'Z';
}

/* ------------------------------------------------------------ тексты */
static const char *const k_texts[] = {
    "", "0", "450", "1234567890", "-12.5", "Паяльник", "Отсос", "ВЫКЛ", "Предсон", "Спит", "10:59", "600:00",
    "Внимание! Режим Expert", "Настройка Паяльник", "Bzzz  ON", "PresleepTime  60", "Заставка  2",
    "Обрыв нагревателя", "КЗ RTD", "ERR ADS1220", "ЁЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮ", "ёйцукенгшщзхъфывапролджэячсмитьбю",
    "A B", "AB CWZ", "WWW", "ZZZ", "TUVXY", "UU", "TT", "VV A", "YT", "xTUVWYZ", "A\xC3", "\xFF\xFE", "\xE2\x82", "€ unknown", "日本", "\xF0\x9F\x98\x80",
    "~!@#$%^&*()_+", "The quick brown fox jumps over the lazy dog 0123456789 0123456789 0123456789",
};
#define N_TEXTS (sizeof k_texts / sizeof k_texts[0])

static const font_t *pick_font(void)
{
    switch (rnd() % 6U) {
        case 0: return &AntiquaB_18_uni;
        case 1: return &AntiquaB_24_uni;
        case 2: return &AntiquaB_32_uni;
        case 3: return &Comic_60_dig;
        default: return &syn_font;
    }
}

static const char *pick_text(char *scratch, size_t n)
{
    if (chance(80)) return k_texts[rnd() % N_TEXTS];
    size_t len = rnd() % (n - 1);
    for (size_t i = 0; i < len; i++) scratch[i] = (char)(chance(70) ? 0x20 + rnd() % 0x5F : rnd() & 0xFF);
    scratch[len] = '\0';
    return scratch;
}

static uint16_t pick_x(void)
{
    unsigned r = rnd() % 100U;
    if (r < 55) return (uint16_t)(rnd() % 320U);
    if (r < 70) return (uint16_t)(rnd() % 12U);          /* у левого края (cell_left < 0 уходит за экран) */
    if (r < 90) return (uint16_t)(300U + rnd() % 30U);   /* у правого края и за ним */
    return (uint16_t)rnd();
}

static const char *state_name(Gfx_JobState_t s)
{
    switch (s) {
        case GFX_JOB_IDLE: return "IDLE"; case GFX_JOB_BUSY: return "BUSY"; case GFX_JOB_DONE: return "DONE";
        case GFX_JOB_ERROR_RETRY: return "RETRY"; case GFX_JOB_ERROR_FATAL: return "FATAL"; default: return "?";
    }
}

int main(int argc, char **argv)
{
    long iters = (argc > 1) ? atol(argv[1]) : 20000;
    uint64_t seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1;
    if (getenv("DUMP")) g_dump_iter = atol(getenv("DUMP"));
    g_rng ^= seed * 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 8; i++) (void)rnd();
    synthetic_font_init();

    char text_buf[48], prev_buf[48];
    for (g_iter = 0; g_iter < iters; g_iter++) {
        g_busy_pct = chance(50) ? 0 : rnd() % 60U;
        g_fail_window_pct = chance(70) ? 0 : rnd() % 30U;
        g_fail_dma_pct = chance(70) ? 0 : rnd() % 30U;

        const font_t *font = pick_font();
        const char *text = chance(2) ? NULL : pick_text(text_buf, sizeof text_buf);
        const char *prev = chance(30) ? NULL : pick_text(prev_buf, sizeof prev_buf);
        uint16_t x = pick_x(), y = (uint16_t)(rnd() % 200U), px = chance(60) ? x : pick_x();
        if (chance(2)) font = NULL;

        ev("measure", "%u,%u", Gfx_MeasureTextWidth(font, text), Gfx_MeasureTextWidth(font, prev));
        Gfx_JobState_t st = Gfx_DrawTextStart(x, y, text, px, prev, font, (display_color_t)rnd(), (display_color_t)rnd());
        ev("start", "%s", state_name(st));
        if (chance(3)) { /* новое задание поверх незавершённого — должно быть отклонено BUSY */
            ev("start2", "%s", state_name(Gfx_DrawTextStart(x, y, "x", x, NULL, &AntiquaB_18_uni, 1, 2)));
        }

        for (int k = 0; k < 400; k++) {
            if (chance(1)) { Gfx_CancelJob(); ev("cancel", ""); }
            st = Gfx_Process();
            ev("proc", "%s", state_name(st));
            if (st != GFX_JOB_BUSY) {
                /* RETRY — задание остаётся в RETRY; повторный старт проверит, что оно перезапускается */
                break;
            }
        }
        if (st == GFX_JOB_BUSY) { Gfx_CancelJob(); ev("cancel_end", ""); }
        ev("after", "%s", state_name(Gfx_Process()));
    }
    printf("RESULT iters=%ld seed=%llu hash=%016llx events=%llu\n", iters, (unsigned long long)seed,
           (unsigned long long)g_hash, (unsigned long long)g_events);
    return 0;
}
