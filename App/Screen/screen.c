/**
 * @file screen.c
 * @brief Реализация screen.h — см. правила в шапке заголовка.
 *
 * Разметка 320x240:
 *  - y=0..29   : общая инфозона — сообщения EEPROM (Error_GetInfoZoneMessage()):
 *                транзитное ("сброшено на заводские", 5 сек) либо авария
 *                (весь сеанс); пусто, если показывать нечего. Таймеры сна
 *                тоже сюда позже, TODO.
 *  - x=159..160: вертикальный разделитель — НЕ доходит до строки пресетов
 *                (та зона общая: один набор пресетов на экран, для
 *                активного канала)
 *  - каждая половина (0..158 / 161..319):
 *      - заголовок канала ("Паяльник"/"Отсос"), шрифт AntiquaB_18_uni —
 *        красный (COLOR_FAULT), если Error_IsChannelFaulted(ch), иначе
 *        обычная активная/неактивная окраска
 *      - текущая температура (LINE_x_CURRENT), шрифт Comic_60_dig — И
 *        сообщение об обрыве (LINE_x_FAULT_MSG), шрифт AntiquaB_18_uni,
 *        НА ТОЙ ЖЕ позиции — это ДВА РАЗНЫХ поля (Comic_60_dig кириллицу
 *        физически не содержит, нельзя вывести текст тем же полем), но в
 *        любой момент содержимое имеет ровно одно из двух, второе пустое
 *        (см. update_channel_content()): обычный случай — число в CURRENT,
 *        FAULT_MSG пуст; RTD/нагреватель оборван — CURRENT пуст, в
 *        FAULT_MSG текст "Обрыв RTD"/"Обрыв нагревателя"
 *      - целевая температура прямо под ней, шрифт AntiquaB_18_uni, всегда
 *        числом независимо от неисправности (ВРЕМЕННО — по ТЗ будет
 *        убрана позже)
 *  - строка пресетов внизу, шрифт AntiquaB_24_uni, ТРИ отдельных поля
 *    (не одна строка) — пресеты активного канала:
 *      - preset1: TextField_Printf(), фиксированный x=10 от левого края
 *      - preset2: TextField_PrintfCentered() вокруг оси разделителя (x=160)
 *      - preset3: TextField_PrintfRightAligned() — правый край в 10px от
 *        правого края экрана (320-10=310)
 *    preset2/preset3 пересчитывают x по факту при каждом изменении значения.
 *
 * Координаты — приближённый вариант по вертикали (не откалиброван визуально
 * на реальном дисплее из этой сессии), по горизонтали — динамическое
 * центрирование/выравнивание по факту измеренной ширины (см. gfx.c/text_field.c).
 */

#include "screen.h"
#include "text_field.h"
#include "display.h"
#include "fonts.h"
#include "channel.h"
#include "state.h"
#include "settings.h"
#include "fsm.h"
#include "error.h"
#include <stddef.h>
#include "fixed_point.h"
#include <stdint.h>
#include <stdbool.h>

/* ---- Индексы строк TextField ---- */
enum {
    LINE_INFO = 0,
    LINE_SOLDER_TITLE,
    LINE_SOLDER_CURRENT,      /* число, шрифт Comic_60_dig — пусто, если неисправность с сообщением */
    LINE_SOLDER_FAULT_MSG,    /* "Обрыв RTD"/"Обрыв нагревателя", шрифт AntiquaB_18_uni — пусто в норме */
    LINE_SOLDER_TARGET,
    LINE_DESOLDER_TITLE,
    LINE_DESOLDER_CURRENT,
    LINE_DESOLDER_FAULT_MSG,
    LINE_DESOLDER_TARGET,
    LINE_PRESET_1,
    LINE_PRESET_2,
    LINE_PRESET_3,
};

/* ---- Геометрия ---- */
#define SCREEN_WIDTH        (320U)
#define SCREEN_HEIGHT       (240U)
#define SCREEN_INFO_HEIGHT  (30U)
#define SCREEN_DIVIDER_X0   (159U)
#define SCREEN_DIVIDER_X1   (160U) /* разделитель 2px шириной: X0..X1 включительно */

#define SCREEN_TITLE_Y      (36U)
#define SCREEN_TITLE_HEIGHT (18U) /* высота шрифта AntiquaB_18_uni */
#define SCREEN_TITLE_LEFT_X   (40U)
#define SCREEN_TITLE_RIGHT_X  (205U)

/* Центры половин экрана — используются TextField_PrintfCentered() для
 * температур, реальная ширина текста меряется на лету (не типовая). */
#define SCREEN_HALF_CENTER_LEFT_X   ((SCREEN_DIVIDER_X0) / 2U)
#define SCREEN_HALF_CENTER_RIGHT_X  (SCREEN_DIVIDER_X1 + 1U + SCREEN_HALF_CENTER_LEFT_X)

#define SCREEN_PRESETS_Y (210U)
/* Пресеты:
 *  - preset1: 10px от левого края (TextField_Printf, фиксированный x)
 *  - preset2: центрирован на оси разделителя (TextField_PrintfCentered)
 *  - preset3: правый край в 10px от правого края экрана (TextField_PrintfRightAligned) */
#define SCREEN_PRESET1_X (10U)
#define SCREEN_PRESET2_CENTER_X (SCREEN_DIVIDER_X0 + 1U)
#define SCREEN_PRESET3_RIGHT_EDGE_X (SCREEN_WIDTH - 10U)

/* Разделитель НЕ доходит до строки пресетов — та зона общая (одна строка
 * на экран). Останавливаем линию с небольшим отступом сверху от SCREEN_PRESETS_Y. */
#define SCREEN_DIVIDER_Y1   (SCREEN_PRESETS_Y - 6U)

/* Текущая+целевая температура центрируются по вертикали в промежутке между
 * низом заголовка и верхом строки пресетов (тот же отступ 6px, что и у
 * разделителя). CURRENT_HEIGHT/TARGET_HEIGHT — высоты шрифтов, GAP — зазор. */
#define SCREEN_CURRENT_HEIGHT (67U)
#define SCREEN_TARGET_HEIGHT  (18U)
#define SCREEN_TEMP_GAP       (10U)
#define SCREEN_TEMP_BAND_TOP    (SCREEN_TITLE_Y + SCREEN_TITLE_HEIGHT + 2U)
#define SCREEN_TEMP_BAND_BOTTOM (SCREEN_DIVIDER_Y1)
#define SCREEN_TEMP_BLOCK_HEIGHT (SCREEN_CURRENT_HEIGHT + SCREEN_TEMP_GAP + SCREEN_TARGET_HEIGHT)
#define SCREEN_CURRENT_Y ((uint16_t)(SCREEN_TEMP_BAND_TOP + \
    ((SCREEN_TEMP_BAND_BOTTOM - SCREEN_TEMP_BAND_TOP) - SCREEN_TEMP_BLOCK_HEIGHT) / 2U))
#define SCREEN_TARGET_Y  ((uint16_t)(SCREEN_CURRENT_Y + SCREEN_CURRENT_HEIGHT + SCREEN_TEMP_GAP))

#define SCREEN_INFO_X (10U)
#define SCREEN_INFO_Y (6U)

/* ---- Цвета ---- */
#define COLOR_BG               DISPLAY_RGB565(0, 0, 0)
#define COLOR_ACTIVE_CURRENT   DISPLAY_RGB565(255, 255, 255)
#define COLOR_INACTIVE_CURRENT DISPLAY_RGB565(90, 90, 90)
#define COLOR_ACTIVE_TARGET    DISPLAY_RGB565(180, 180, 180)
#define COLOR_INACTIVE_TARGET  DISPLAY_RGB565(60, 60, 60)
#define COLOR_ACTIVE_PRESETS   DISPLAY_RGB565(255, 210, 0)
#define COLOR_ACTIVE_TITLE     DISPLAY_RGB565(255, 255, 255)
#define COLOR_INACTIVE_TITLE   DISPLAY_RGB565(90, 90, 90)
#define COLOR_FAULT            DISPLAY_RGB565(255, 40, 40) /* заголовок/текущая температура при неисправности инструмента */
#define COLOR_DIVIDER          DISPLAY_RGB565(100, 100, 100)
#define COLOR_INFO             DISPLAY_RGB565(255, 255, 255)

static channel_id_t s_last_active_channel;
static bool s_last_fault[CHANNEL_COUNT]; /* чтобы перекрашивать title/current только при реальном изменении неисправности */

/**
 * @brief Применить цвета title/current канала. Приоритет: неисправность
 *        (красный) > активный/неактивный. Target сюда не входит — он
 *        всегда числом и обычной активной/неактивной окраской (ВРЕМЕННО,
 *        см. докстринг файла). Пресеты тоже не входят — общие поля.
 */
static void apply_channel_colors(channel_id_t ch)
{
    uint8_t line_title, line_current;
    bool active = (ch == InputFSM_GetActiveChannel());
    bool faulted = Error_IsChannelFaulted(ch);

    if (ch == CHANNEL_SOLDER) {
        line_title   = LINE_SOLDER_TITLE;
        line_current = LINE_SOLDER_CURRENT;
    } else {
        line_title   = LINE_DESOLDER_TITLE;
        line_current = LINE_DESOLDER_CURRENT;
    }

    if (faulted) {
        TextField_SetColors(line_title,   COLOR_FAULT, COLOR_BG);
        TextField_SetColors(line_current, COLOR_FAULT, COLOR_BG);
    } else {
        TextField_SetColors(line_title,   active ? COLOR_ACTIVE_TITLE   : COLOR_INACTIVE_TITLE,   COLOR_BG);
        TextField_SetColors(line_current, active ? COLOR_ACTIVE_CURRENT : COLOR_INACTIVE_CURRENT, COLOR_BG);
    }
}

static void apply_target_colors(channel_id_t ch)
{
    uint8_t line_target = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_TARGET : LINE_DESOLDER_TARGET;
    bool active = (ch == InputFSM_GetActiveChannel());
    TextField_SetColors(line_target, active ? COLOR_ACTIVE_TARGET : COLOR_INACTIVE_TARGET, COLOR_BG);
}

/**
 * @brief Нарисовать статический вертикальный разделитель (однократно, блокирующе)
 */
static void draw_divider(void)
{
    uint16_t width = (uint16_t)(SCREEN_DIVIDER_X1 - SCREEN_DIVIDER_X0 + 1);
    uint16_t height = (uint16_t)(SCREEN_DIVIDER_Y1 - SCREEN_INFO_HEIGHT + 1);

    if (Display_SetWindow(SCREEN_DIVIDER_X0, SCREEN_INFO_HEIGHT,
                           SCREEN_DIVIDER_X1, SCREEN_DIVIDER_Y1) == DISPLAY_OK) {
        Display_FillColorDMA(COLOR_DIVIDER, (uint32_t)width * height);
        while (Display_IsBusy()) { } /* однократно, при старте, до входа в главный цикл */
    }
}

void Screen_Init(void)
{
    draw_divider();

    TextField_ConfigureLine(LINE_INFO, SCREEN_INFO_X, SCREEN_INFO_Y,
                             &AntiquaB_16_uni, COLOR_INFO, COLOR_BG);

    TextField_ConfigureLine(LINE_SOLDER_TITLE, SCREEN_TITLE_LEFT_X, SCREEN_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_ACTIVE_TITLE, COLOR_BG);
    /* CURRENT остаётся Comic_60_dig, как и было — крупные цифры. Кириллицу
     * этот шрифт не содержит физически, поэтому для текста об обрыве
     * заведено ОТДЕЛЬНОЕ поле FAULT_MSG (AntiquaB_18_uni), на той же
     * позиции — в любой момент содержимое имеет ровно одно из двух полей,
     * второе пустое (см. update_channel_content()). */
    TextField_ConfigureLine(LINE_SOLDER_CURRENT, SCREEN_HALF_CENTER_LEFT_X, SCREEN_CURRENT_Y,
                             &Comic_60_dig, COLOR_ACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_FAULT_MSG, SCREEN_HALF_CENTER_LEFT_X, SCREEN_CURRENT_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_TARGET, SCREEN_HALF_CENTER_LEFT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_ACTIVE_TARGET, COLOR_BG);

    TextField_ConfigureLine(LINE_DESOLDER_TITLE, SCREEN_TITLE_RIGHT_X, SCREEN_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_INACTIVE_TITLE, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_CURRENT, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_CURRENT_Y,
                             &Comic_60_dig, COLOR_INACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_FAULT_MSG, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_CURRENT_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_TARGET, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_INACTIVE_TARGET, COLOR_BG);

    TextField_ConfigureLine(LINE_PRESET_1, SCREEN_PRESET1_X, SCREEN_PRESETS_Y,
                             &AntiquaB_24_uni, COLOR_ACTIVE_PRESETS, COLOR_BG);
    TextField_ConfigureLine(LINE_PRESET_2, SCREEN_PRESET2_CENTER_X, SCREEN_PRESETS_Y,
                             &AntiquaB_24_uni, COLOR_ACTIVE_PRESETS, COLOR_BG);
    TextField_ConfigureLine(LINE_PRESET_3, SCREEN_PRESET3_RIGHT_EDGE_X, SCREEN_PRESETS_Y,
                             &AntiquaB_24_uni, COLOR_ACTIVE_PRESETS, COLOR_BG);
    /* x, переданный здесь для CURRENT/TARGET/PRESET_2/PRESET_3, — просто
     * начальное значение, реальная позиция пересчитывается по факту при
     * первом же TextField_PrintfCentered()/PrintfRightAligned() в
     * Screen_Update(), до первой отрисовки на экран. */

    /* Подписи каналов — статичный текст, меняется только цвет */
    TextField_Printf(LINE_SOLDER_TITLE, "Паяльник");
    TextField_Printf(LINE_DESOLDER_TITLE, "Отсос");

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_last_fault[ch] = false; /* Error_Init() тоже гарантирует "нет неисправности" по умолчанию */
    }
    /* Solder активен по умолчанию при старте (см. InputFSM_Init()) —
     * начальные цвета выше уже расставлены соответственно. */
    s_last_active_channel = CHANNEL_SOLDER;
}

/**
 * @brief Обновить содержимое канала: title-цвет, current/fault_msg (ровно
 *        одно из двух непусто), target (всегда число)
 * @param center_x Центр половины экрана этого канала
 */
static void update_channel_content(channel_id_t ch, uint16_t center_x)
{
    uint8_t line_current   = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_CURRENT   : LINE_DESOLDER_CURRENT;
    uint8_t line_fault_msg = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_FAULT_MSG : LINE_DESOLDER_FAULT_MSG;
    uint8_t line_target    = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_TARGET   : LINE_DESOLDER_TARGET;

    const char *fault_msg = Error_GetChannelFaultMessage(ch); /* не NULL только для RTD_OPEN/HEATER_OPEN */

    if (fault_msg != NULL) {
        /* Текущая температура НЕ выводится вообще — на её месте сообщение
         * в отдельном поле (Comic_60_dig кириллицу не содержит) */
        TextField_PrintfCentered(line_current, center_x, "");
        TextField_PrintfCentered(line_fault_msg, center_x, "%s", fault_msg);
    } else {
        fixed_t cur = State_GetCurrentTemp(ch);
        int32_t cur_int = FIXED_TO_INT(cur);
        TextField_PrintfCentered(line_current, center_x, "%ld", (long)cur_int);
        TextField_PrintfCentered(line_fault_msg, center_x, "");
    }

    /* Целевая — всегда числом, независимо от неисправности (ВРЕМЕННО, см. докстринг) */
    uint16_t target = Settings_GetTarget(ch);
    TextField_PrintfCentered(line_target, center_x, "%u", (unsigned)target);

    bool faulted = Error_IsChannelFaulted(ch);
    if (faulted != s_last_fault[ch]) {
        apply_channel_colors(ch);
        s_last_fault[ch] = faulted;
    }
}

void Screen_Update(void)
{
    update_channel_content(CHANNEL_SOLDER, SCREEN_HALF_CENTER_LEFT_X);
    update_channel_content(CHANNEL_DESOLDER, SCREEN_HALF_CENTER_RIGHT_X);

    channel_id_t active = InputFSM_GetActiveChannel();

    /* Пресеты — три отдельных поля, всегда показывают пресеты АКТИВНОГО канала */
    TextField_Printf(LINE_PRESET_1, "%u", (unsigned)Settings_GetPreset(active, PRESET_1));
    TextField_PrintfCentered(LINE_PRESET_2, SCREEN_PRESET2_CENTER_X, "%u", (unsigned)Settings_GetPreset(active, PRESET_2));
    TextField_PrintfRightAligned(LINE_PRESET_3, SCREEN_PRESET3_RIGHT_EDGE_X, "%u", (unsigned)Settings_GetPreset(active, PRESET_3));

    if (active != s_last_active_channel) {
        apply_channel_colors(s_last_active_channel);
        apply_target_colors(s_last_active_channel);
        apply_channel_colors(active);
        apply_target_colors(active);
        s_last_active_channel = active;
    }

    const char *info_msg = Error_GetInfoZoneMessage();
    TextField_Printf(LINE_INFO, "%s", (info_msg != NULL) ? info_msg : "");
}
