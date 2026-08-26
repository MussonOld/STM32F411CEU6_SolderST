/**
 * @file screen.c
 * @brief Реализация screen.h — см. правила в шапке заголовка.
 *
 * Разметка 320x240:
 *  - y=0..29   : общая инфозона, три поля: слева таймер сна паяльника,
 *                справа — отсоса (Sleep_GetMode()/Sleep_GetRemainingSeconds()),
 *                по центру — сообщение EEPROM (Error_GetInfoZoneMessage()):
 *                транзитное ("сброшено на заводские", 5 сек) либо авария
 *                (весь сеанс); пусто, если показывать нечего.
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
 *  - Сервисное меню (InputFSM_GetScreenMode()==SCREEN_MODE_SERVICE) —
 *    ПОЛНОСТЬЮ заменяет собой всё вышеописанное (render_menu(), отдельная
 *    ветка в Screen_Update()). Заголовок ("Настройка Паяльник"/"Настройка
 *    Отсос" — Menu_GetTitle()) + до 7 строк списком друг под другом,
 *    шрифт AntiquaB_18_uni везде. Выбранный пункт подсвечивается цветом
 *    (жёлтый — выбран, красный — редактируется), не текстовым курсором.
 *    Те же 7 строк переиспользуются для трёх строк предупреждения Expert
 *    (Menu_IsShowingExpertWarning()) — отдельных полей под это не заведено.
 *    При КАЖДОЙ смене режима экрана (главный <-> меню, в обе стороны) —
 *    полная заливка фона + TextField_InvalidateAll() (см.
 *    clear_screen_for_mode_switch()), чтобы не было наложения одного
 *    экрана на остатки другого; статический разделитель — часть только
 *    главного экрана, перерисовывается заново при возврате в него.
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
#include "sleep.h"
#include "menu.h"
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
    LINE_INFO_SLEEP_SOLDER,   /* инфозона слева — таймер сна паяльника */
    LINE_INFO_SLEEP_DESOLDER, /* инфозона справа — таймер сна отсоса */
    LINE_MENU_TITLE,          /* сервисное меню — "Настройка Паяльник/Отсос" */
    LINE_MENU_ITEM_0,         /* сервисное меню — 7 строк списка (максимум для уровня Expert);
                                 при MENU_STATE_EXPERT_WARNING строки 0-2 заняты текстом
                                 предупреждения, см. render_menu() */
    LINE_MENU_ITEM_1,
    LINE_MENU_ITEM_2,
    LINE_MENU_ITEM_3,
    LINE_MENU_ITEM_4,
    LINE_MENU_ITEM_5,
    LINE_MENU_ITEM_6,
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
/* Инфозона разбита на три поля по x: sleep-таймер паяльника слева,
 * сообщение EEPROM по центру, sleep-таймер отсоса справа. Если сообщение
 * EEPROM длинное — может визуально наехать на соседние поля; это
 * приближённый первый вариант, поправить координаты по факту на экране. */
#define SCREEN_INFO_SLEEP_SOLDER_X   (10U)
#define SCREEN_INFO_EEPROM_X         (100U)
#define SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X (SCREEN_WIDTH - 10U)

/* ---- Экран сервисного меню (заменяет собой весь главный экран целиком,
 * пока InputFSM_GetScreenMode() == SCREEN_MODE_SERVICE) ---- */
#define SCREEN_MENU_TITLE_X   (20U)
#define SCREEN_MENU_TITLE_Y   (14U)
#define SCREEN_MENU_ITEM_X    (20U)
#define SCREEN_MENU_ITEM_Y0   (50U)
#define SCREEN_MENU_ITEM_STEP (26U)
#define SCREEN_MENU_ITEM_ROWS (7U) /* максимум пунктов — уровень Expert */

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

#define COLOR_MENU_TITLE    DISPLAY_RGB565(255, 255, 255)
#define COLOR_MENU_NORMAL   DISPLAY_RGB565(180, 180, 180)
#define COLOR_MENU_CURSOR   DISPLAY_RGB565(255, 210, 0)  /* выбранный пункт, не редактируется */
#define COLOR_MENU_EDITING  DISPLAY_RGB565(255, 80, 80)  /* выбранный пункт, редактируется прямо сейчас */

static channel_id_t s_last_active_channel;
static bool s_last_fault[CHANNEL_COUNT]; /* чтобы перекрашивать title/current только при реальном изменении неисправности */
static screen_mode_t s_last_screen_mode; /* чтобы очищать экран только при реальной смене режима, не каждый кадр */

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
        while (Display_IsBusy()) { } /* однократно, блокирующе — редкое событие (старт/смена режима экрана), не каждый кадр */
    }
}

/**
 * @brief Полностью стереть экран и заставить TextField перерисовать всё
 *        заново — вызывается при КАЖДОЙ смене режима экрана (главный <->
 *        сервисное меню, в обе стороны), чтобы не было наложения одного
 *        экрана на остатки другого (разные поля/раскладка, разный набор
 *        используемых строк). Блокирует ненадолго (редкое событие, не
 *        каждый кадр — аналогично draw_divider()).
 */
static void clear_screen_for_mode_switch(screen_mode_t new_mode)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1) == DISPLAY_OK) {
        Display_FillColorDMA(COLOR_BG, (uint32_t)SCREEN_WIDTH * SCREEN_HEIGHT);
        while (Display_IsBusy()) { }
    }
    if (new_mode == SCREEN_MODE_MAIN) {
        draw_divider(); /* стёрли вместе со всем экраном — у главного экрана он статический, рисуем заново */
    }
    TextField_InvalidateAll(); /* все строки (обоих экранов) забывают, что было на экране — перерисуются с нуля на чистом фоне */
}

void Screen_Init(void)
{
    draw_divider();

    TextField_ConfigureLine(LINE_INFO, SCREEN_INFO_EEPROM_X, SCREEN_INFO_Y,
                             &AntiquaB_16_uni, COLOR_INFO, COLOR_BG);
    TextField_ConfigureLine(LINE_INFO_SLEEP_SOLDER, SCREEN_INFO_SLEEP_SOLDER_X, SCREEN_INFO_Y,
                             &AntiquaB_16_uni, COLOR_INFO, COLOR_BG);
    TextField_ConfigureLine(LINE_INFO_SLEEP_DESOLDER, SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X, SCREEN_INFO_Y,
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

    /* Сервисное меню — шрифт 18 везде (см. спецификацию), позиции по
     * вертикали друг под другом, x общий для title и всех строк списка */
    TextField_ConfigureLine(LINE_MENU_TITLE, SCREEN_MENU_TITLE_X, SCREEN_MENU_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_MENU_TITLE, COLOR_BG);
    for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
        TextField_ConfigureLine((uint8_t)(LINE_MENU_ITEM_0 + i), SCREEN_MENU_ITEM_X,
                                 (uint16_t)(SCREEN_MENU_ITEM_Y0 + i * SCREEN_MENU_ITEM_STEP),
                                 &AntiquaB_18_uni, COLOR_MENU_NORMAL, COLOR_BG);
    }

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_last_fault[ch] = false; /* Error_Init() тоже гарантирует "нет неисправности" по умолчанию */
    }
    /* Solder активен по умолчанию при старте (см. InputFSM_Init()) —
     * начальные цвета выше уже расставлены соответственно. */
    s_last_active_channel = CHANNEL_SOLDER;
    s_last_screen_mode = SCREEN_MODE_MAIN; /* совпадает с InputFSM_Init() — при первом Screen_Update() лишней очистки не будет */
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

/**
 * @brief Обновить поле таймера сна одного канала в инфозоне
 *
 * AWAKE (простаивает, идёт Таймер 1)    -> "До сна MM:SS"
 * AWAKE (не простаивает / выключено)    -> "" (пусто)
 * PRESLEEP (идёт Таймер 2)              -> "Предсон MM:SS"
 * PRESLEEP (SleepTimeout выключен)      -> "Предсон" (без времени — бессрочно)
 * SLEEP                                  -> "Спит"
 *
 * @param right_aligned false — TextField_Printf с фиксированным x (левое
 *        поле, паяльник); true — TextField_PrintfRightAligned (правое поле,
 *        отсос, правый край фиксирован независимо от длины текста)
 */
static void update_sleep_status(channel_id_t ch, uint8_t line, bool right_aligned)
{
    sleep_mode_t mode = Sleep_GetMode(ch);
    uint32_t remaining = Sleep_GetRemainingSeconds(ch);
    uint32_t min = remaining / 60U;
    uint32_t sec = remaining % 60U;

    if (right_aligned) {
        switch (mode) {
            case SLEEP_MODE_AWAKE:
                if (remaining == 0) TextField_PrintfRightAligned(line, SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X, "");
                else TextField_PrintfRightAligned(line, SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X, "До сна %lu:%02lu", (unsigned long)min, (unsigned long)sec);
                break;
            case SLEEP_MODE_PRESLEEP:
                if (remaining == 0) TextField_PrintfRightAligned(line, SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X, "Предсон");
                else TextField_PrintfRightAligned(line, SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X, "Предсон %lu:%02lu", (unsigned long)min, (unsigned long)sec);
                break;
            case SLEEP_MODE_SLEEP:
                TextField_PrintfRightAligned(line, SCREEN_INFO_SLEEP_DESOLDER_RIGHT_EDGE_X, "Спит");
                break;
        }
    } else {
        switch (mode) {
            case SLEEP_MODE_AWAKE:
                if (remaining == 0) TextField_Printf(line, "");
                else TextField_Printf(line, "До сна %lu:%02lu", (unsigned long)min, (unsigned long)sec);
                break;
            case SLEEP_MODE_PRESLEEP:
                if (remaining == 0) TextField_Printf(line, "Предсон");
                else TextField_Printf(line, "Предсон %lu:%02lu", (unsigned long)min, (unsigned long)sec);
                break;
            case SLEEP_MODE_SLEEP:
                TextField_Printf(line, "Спит");
                break;
        }
    }
}

/**
 * @brief Отрисовать экран сервисного меню целиком (заменяет главный экран)
 */
static void render_menu(void)
{
    TextField_Printf(LINE_MENU_TITLE, "%s", Menu_GetTitle());

    if (Menu_IsShowingExpertWarning()) {
        for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
            uint8_t line = (uint8_t)(LINE_MENU_ITEM_0 + i);
            if (i < 3) {
                TextField_Printf(line, "%s", Menu_GetExpertWarningLine(i));
            } else {
                TextField_Printf(line, "");
            }
            TextField_SetColors(line, COLOR_MENU_NORMAL, COLOR_BG);
        }
        return;
    }

    uint8_t count = Menu_GetItemCount();
    uint8_t cursor = Menu_GetCursor();
    bool editing = Menu_IsEditing();

    for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
        uint8_t line = (uint8_t)(LINE_MENU_ITEM_0 + i);

        if (i < count) {
            char value[16];
            Menu_GetItemValueText(i, value, sizeof(value));
            if (value[0] != '\0') {
                TextField_Printf(line, "%s  %s", Menu_GetItemLabel(i), value);
            } else {
                TextField_Printf(line, "%s", Menu_GetItemLabel(i));
            }
        } else {
            TextField_Printf(line, ""); /* уровень User короче Expert — лишние строки пустые */
        }

        display_color_t color;
        if (i == cursor) {
            color = editing ? COLOR_MENU_EDITING : COLOR_MENU_CURSOR;
        } else {
            color = COLOR_MENU_NORMAL;
        }
        TextField_SetColors(line, color, COLOR_BG);
    }
}

void Screen_Update(void)
{
    screen_mode_t mode = InputFSM_GetScreenMode();
    if (mode != s_last_screen_mode) {
        clear_screen_for_mode_switch(mode);
        s_last_screen_mode = mode;
    }

    if (mode == SCREEN_MODE_SERVICE) {
        render_menu();
        return; /* меню заменяет собой весь главный экран — остальное не обновляем */
    }

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

    update_sleep_status(CHANNEL_SOLDER, LINE_INFO_SLEEP_SOLDER, false);
    update_sleep_status(CHANNEL_DESOLDER, LINE_INFO_SLEEP_DESOLDER, true);
}
