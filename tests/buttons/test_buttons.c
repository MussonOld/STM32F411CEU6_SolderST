/**
 * @file test_buttons.c
 * @brief Хост-тесты buttons.c: симулируется GPIOA->IDR и время (опрос каждые
 *        10 мс, как в main.c), проверяется последовательность событий.
 *
 * Модель времени: press()/release() меняют пины между опросами; wait_ms(n)
 * делает n/10 опросов. Кнопка подтверждается антидребезгом через 3 опроса,
 * поэтому "одновременно" = изменение в одном и том же опросе.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "buttons.h"
#include "main.h"

/* ---- заглушки окружения ---- */
GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc;
static uint32_t g_tick;
uint32_t HAL_GetTick(void) { return g_tick; }

static const uint16_t k_pin[BUTTON_COUNT] = {
    [BUTTON_SET1] = SET1_Pin, [BUTTON_SET2] = SET2_Pin, [BUTTON_SET3] = SET3_Pin,
    [BUTTON_DN]   = DN_Pin,   [BUTTON_UP]   = UP_Pin,   [BUTTON_TOOLS] = TOOLS_Pin,
};

#define M_SET1  BUTTON_MASK(BUTTON_SET1)
#define M_SET2  BUTTON_MASK(BUTTON_SET2)
#define M_SET3  BUTTON_MASK(BUTTON_SET3)
#define M_DN    BUTTON_MASK(BUTTON_DN)
#define M_UP    BUTTON_MASK(BUTTON_UP)
#define M_TOOLS BUTTON_MASK(BUTTON_TOOLS)
#define M_CHORD BUTTONS_CHORD_UP_DN_MASK

/* ---- симулятор ---- */
#define MAX_EV 64
static button_event_t g_ev[MAX_EV];
static int g_nev;

static void begin(uint32_t tick0)
{
    test_gpioa.IDR = 0xFFFFu;   /* всё отпущено (активный уровень низкий) */
    g_tick = tick0;
    g_nev = 0;
    Buttons_Init();
}

static void press(uint8_t mask)
{
    for (int i = 0; i < BUTTON_COUNT; i++)
        if (mask & BUTTON_MASK(i)) test_gpioa.IDR &= (uint32_t)~k_pin[i];
}

static void release(uint8_t mask)
{
    for (int i = 0; i < BUTTON_COUNT; i++)
        if (mask & BUTTON_MASK(i)) test_gpioa.IDR |= k_pin[i];
}

static void poll_collect(int pop)
{
    Buttons_Poll();
    if (pop) {
        button_event_t e;
        while (Buttons_PopEvent(&e)) {
            if (g_nev < MAX_EV) g_ev[g_nev++] = e;
        }
    }
    g_tick += BUTTONS_POLL_MS;
}

static void wait_ms(unsigned ms)
{
    for (unsigned i = 0; i < ms / BUTTONS_POLL_MS; i++) poll_collect(1);
}

/* ---- проверки ---- */
static int g_fail, g_total;

static const char *ev_name(button_event_type_t t)
{
    switch (t) {
        case BUTTON_EVENT_SHORT_PRESS: return "SHORT";
        case BUTTON_EVENT_LONG_PRESS:  return "LONG";
        case BUTTON_EVENT_CHORD_SHORT: return "CHORD_SHORT";
        case BUTTON_EVENT_CHORD_LONG:  return "CHORD_LONG";
        case BUTTON_EVENT_VIOLATION:   return "VIOLATION";
    }
    return "?";
}

static void dump(char *out, size_t n, const button_event_t *e, int cnt)
{
    out[0] = 0;
    for (int i = 0; i < cnt; i++) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, "%s%s(%02x)", i ? ", " : "", ev_name(e[i].type), e[i].mask);
    }
    if (cnt == 0) snprintf(out, n, "(нет событий)");
}

#define EV(t, m) { (t), (uint8_t)(m) }
#define SHORT(m) EV(BUTTON_EVENT_SHORT_PRESS, m)
#define LONG_(m) EV(BUTTON_EVENT_LONG_PRESS, m)
#define CSHORT   EV(BUTTON_EVENT_CHORD_SHORT, M_CHORD)
#define CLONG    EV(BUTTON_EVENT_CHORD_LONG, M_CHORD)
#define VIOL(m)  EV(BUTTON_EVENT_VIOLATION, m)

#define EXPECT(name, ...)                                                        \
    do {                                                                         \
        const button_event_t exp_[] = { __VA_ARGS__ };                           \
        int n_ = (int)(sizeof(exp_) / sizeof(exp_[0]));                          \
        expect_events(name, exp_, n_);                                           \
    } while (0)
#define EXPECT_NONE(name) expect_events(name, NULL, 0)

static void expect_events(const char *name, const button_event_t *exp, int n)
{
    int ok = (g_nev == n);
    for (int i = 0; ok && i < n; i++)
        ok = (g_ev[i].type == exp[i].type && g_ev[i].mask == exp[i].mask);
    g_total++;
    if (ok) {
        printf("  ok    %s\n", name);
    } else {
        char a[512], b[512];
        dump(a, sizeof a, exp, n);
        dump(b, sizeof b, g_ev, g_nev);
        printf("  FAIL  %s\n          ожидалось: %s\n          получено:  %s\n", name, a, b);
        g_fail++;
    }
}

static void expect_true(const char *name, int cond)
{
    g_total++;
    if (cond) printf("  ok    %s\n", name);
    else { printf("  FAIL  %s\n", name); g_fail++; }
}

/* ======================= тесты ======================= */

static void test_single_buttons(void)
{
    puts("одиночные кнопки");
    static const struct { const char *n; uint8_t m; } b[] = {
        {"SET1", M_SET1}, {"SET2", M_SET2}, {"SET3", M_SET3}, {"UP", M_UP}, {"DN", M_DN}, {"TOOLS", M_TOOLS},
    };
    char name[64];
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) {
        begin(1000);
        press(b[i].m); wait_ms(100); release(b[i].m); wait_ms(100);
        snprintf(name, sizeof name, "%s короткое -> SHORT_PRESS", b[i].n);
        EXPECT(name, SHORT(b[i].m));
    }
    for (unsigned i = 0; i < sizeof b / sizeof b[0]; i++) {
        if (b[i].m == M_TOOLS) continue;
        begin(1000);
        press(b[i].m); wait_ms(700); release(b[i].m); wait_ms(100);
        snprintf(name, sizeof name, "%s длинное -> один LONG_PRESS, без SHORT", b[i].n);
        EXPECT(name, LONG_(b[i].m));
    }
    begin(1000);
    press(M_SET1); wait_ms(500); release(M_SET1); wait_ms(100);
    EXPECT("SET1 500 мс -> ещё короткое", SHORT(M_SET1));
}

static void test_chord(void)
{
    puts("аккорд UP+DN");
    begin(1000);
    press(M_UP); wait_ms(50); press(M_DN); wait_ms(100); release(M_UP | M_DN); wait_ms(100);
    EXPECT("каскадом (UP, затем DN) короткий -> CHORD_SHORT", CSHORT);

    begin(1000);
    press(M_DN); wait_ms(50); press(M_UP); wait_ms(100); release(M_UP | M_DN); wait_ms(100);
    EXPECT("каскадом (DN, затем UP) короткий -> CHORD_SHORT", CSHORT);

    begin(1000);
    press(M_UP | M_DN); wait_ms(100); release(M_UP | M_DN); wait_ms(100);
    EXPECT("одновременно (в одном опросе) короткий -> CHORD_SHORT", CSHORT);

    begin(1000);
    press(M_UP | M_DN); wait_ms(700); release(M_UP | M_DN); wait_ms(100);
    EXPECT("одновременно длинный -> один CHORD_LONG", CLONG);

    begin(1000);
    press(M_UP); wait_ms(50); press(M_DN); wait_ms(700); release(M_UP | M_DN); wait_ms(100);
    EXPECT("каскадом длинный -> один CHORD_LONG", CLONG);

    begin(1000);
    press(M_UP | M_DN); wait_ms(100); release(M_UP); wait_ms(100); release(M_DN); wait_ms(100);
    EXPECT("отпускание по одной -> CHORD_SHORT, без нарушения", CSHORT);

    begin(1000);
    press(M_UP | M_DN); wait_ms(700); release(M_UP); wait_ms(100); release(M_DN); wait_ms(100);
    EXPECT("длинный, отпускание по одной -> только CHORD_LONG", CLONG);

    begin(1000);
    press(M_UP); wait_ms(190); press(M_DN); wait_ms(100); release(M_UP | M_DN); wait_ms(100);
    EXPECT("партнёр через 190 мс (в окне) -> аккорд", CSHORT);

    begin(1000);
    press(M_UP); wait_ms(200); press(M_DN); wait_ms(100); release(M_UP | M_DN); wait_ms(100);
    EXPECT("партнёр ровно через 200 мс (граница окна) -> аккорд", CSHORT);

    begin(1000);
    press(M_UP); wait_ms(210); press(M_DN); wait_ms(100); release(M_UP | M_DN); wait_ms(100);
    EXPECT("партнёр через 210 мс (после окна) -> нарушение", VIOL(M_CHORD));
}

static void test_violations(void)
{
    puts("нарушения");
    begin(1000);
    press(M_SET1 | M_SET2); wait_ms(100); release(M_SET1 | M_SET2); wait_ms(100);
    EXPECT("SET1+SET2 одновременно", VIOL(M_SET1 | M_SET2));

    begin(1000);
    press(M_SET1); wait_ms(50); press(M_SET2); wait_ms(100); release(M_SET1 | M_SET2); wait_ms(100);
    EXPECT("SET1, затем SET2", VIOL(M_SET1 | M_SET2));

    begin(1000);
    press(M_SET1 | M_SET2 | M_SET3); wait_ms(100); release(M_SET1 | M_SET2 | M_SET3); wait_ms(100);
    EXPECT("три кнопки сразу", VIOL(M_SET1 | M_SET2 | M_SET3));

    begin(1000);
    press(M_UP | M_SET1); wait_ms(100); release(M_UP | M_SET1); wait_ms(100);
    EXPECT("UP+SET1 одновременно", VIOL(M_UP | M_SET1));

    begin(1000);
    press(M_UP); wait_ms(50); press(M_SET1); wait_ms(100); release(M_UP | M_SET1); wait_ms(100);
    EXPECT("UP, затем SET1 в окне аккорда", VIOL(M_UP | M_SET1));

    begin(1000);
    press(M_TOOLS | M_UP); wait_ms(100); release(M_TOOLS | M_UP); wait_ms(100);
    EXPECT("TOOLS+UP", VIOL(M_TOOLS | M_UP));

    begin(1000);
    press(M_TOOLS); wait_ms(700); release(M_TOOLS); wait_ms(100);
    EXPECT("TOOLS дольше порога -> одно нарушение, без SHORT", VIOL(M_TOOLS));

    begin(1000);
    press(M_UP | M_DN); wait_ms(100); press(M_SET1); wait_ms(100); release(M_UP | M_DN | M_SET1); wait_ms(100);
    EXPECT("аккорд + третья кнопка -> нарушение, без CHORD_SHORT", VIOL(M_UP | M_DN | M_SET1));

    begin(1000);
    press(M_SET1); wait_ms(50); press(M_SET2); wait_ms(100);
    expect_true("IsIgnored() истинно, пока кнопки зажаты", Buttons_IsIgnored());
    release(M_SET1); wait_ms(100);
    expect_true("IsIgnored() истинно, пока зажата хоть одна", Buttons_IsIgnored());
    release(M_SET2); wait_ms(100);
    expect_true("IsIgnored() снято после полного отпускания", !Buttons_IsIgnored());
    g_nev = 0;
    press(M_SET3); wait_ms(100); release(M_SET3); wait_ms(100);
    EXPECT("после нарушения ввод снова работает", SHORT(M_SET3));
}

static void test_debounce_and_held(void)
{
    puts("антидребезг и IsHeld");
    begin(1000);
    press(M_SET1); wait_ms(20); release(M_SET1); wait_ms(100);
    EXPECT_NONE("импульс 20 мс игнорируется");

    begin(1000);
    press(M_SET1); wait_ms(100); release(M_SET1); wait_ms(20); press(M_SET1); wait_ms(100); release(M_SET1); wait_ms(100);
    EXPECT("провал 20 мс внутри удержания не разрывает нажатие", SHORT(M_SET1));

    begin(1000);
    press(M_SET1);
    poll_collect(1); poll_collect(1);
    expect_true("IsHeld() ещё ложно на 2-м опросе", !Buttons_IsHeld(BUTTON_SET1));
    poll_collect(1);
    expect_true("IsHeld() истинно с 3-го опроса", Buttons_IsHeld(BUTTON_SET1));
    expect_true("IsHeld() не затрагивает другие кнопки", !Buttons_IsHeld(BUTTON_SET2));
    release(M_SET1);
    poll_collect(1); poll_collect(1); poll_collect(1);
    expect_true("IsHeld() ложно после отпускания", !Buttons_IsHeld(BUTTON_SET1));
    expect_true("IsHeld() с неверным id -> false", !Buttons_IsHeld(BUTTON_COUNT));

    begin(1000);
    press(M_SET1 | M_SET2); wait_ms(100);
    expect_true("IsHeld() работает и во время нарушения", Buttons_IsHeld(BUTTON_SET1) && Buttons_IsHeld(BUTTON_SET2));
}

static void test_misc(void)
{
    puts("прочее");
    begin(0xFFFFFF00u);
    press(M_SET1); wait_ms(700); release(M_SET1); wait_ms(100);
    EXPECT("длинное нажатие через переполнение HAL_GetTick()", LONG_(M_SET1));

    begin(1000);
    press(M_TOOLS); wait_ms(100); release(M_TOOLS); wait_ms(100);
    press(M_SET1); wait_ms(100); release(M_SET1); wait_ms(100);
    EXPECT("две подряд кнопки -> два события", SHORT(M_TOOLS), SHORT(M_SET1));

    begin(1000);
    for (int i = 0; i < 10; i++) {
        press(M_SET1);
        for (int k = 0; k < 10; k++) poll_collect(0);
        release(M_SET1);
        for (int k = 0; k < 10; k++) poll_collect(0);
    }
    wait_ms(10);
    EXPECT("переполнение очереди (8): лишние события теряются без сбоя",
           SHORT(M_SET1), SHORT(M_SET1), SHORT(M_SET1), SHORT(M_SET1),
           SHORT(M_SET1), SHORT(M_SET1), SHORT(M_SET1), SHORT(M_SET1));
}

static void test_handover(void)
{
    puts("передача с одной кнопки на другую в одном опросе");
    begin(1000);
    press(M_UP); wait_ms(100);
    release(M_UP); press(M_DN);
    wait_ms(100); release(M_DN); wait_ms(100);
    EXPECT("UP -> DN: два коротких нажатия с верными масками", SHORT(M_UP), SHORT(M_DN));

    begin(1000);
    press(M_SET1); wait_ms(100);
    release(M_SET1); press(M_SET2);
    wait_ms(700); release(M_SET2); wait_ms(100);
    EXPECT("SET1 -> SET2 (удержана): SHORT(SET1), затем LONG(SET2)", SHORT(M_SET1), LONG_(M_SET2));

    begin(1000);
    press(M_UP); wait_ms(50);
    release(M_UP); press(M_DN);
    wait_ms(100); release(M_DN); wait_ms(100);
    EXPECT("UP -> DN внутри окна аккорда (без аккорда)", SHORT(M_UP), SHORT(M_DN));
}

int main(void)
{
    test_single_buttons();
    test_chord();
    test_violations();
    test_debounce_and_held();
    test_misc();
    test_handover();
    printf("\nитого: %d проверок, провалено %d\n", g_total, g_fail);
    return g_fail ? 1 : 0;
}
