/**
 * @file screen.c
 * @brief Реализация screen.h — см. правила в шапке заголовка.
 *
 * Разметка 320x240:
 *  - y=0..29   : общая инфозона, три поля: слева иконка+таймер сна
 *                паяльника (справа от иконки, у своего края — перед
 *                разделителем), справа — иконка+таймер отсоса (у правого
 *                края экрана), Sleep_GetMode()/Sleep_GetRemainingSeconds(),
 *                шрифт AntiquaB_18_uni, цвет по режиму (белый=AWAKE,
 *                жёлтый=PRESLEEP, красный=SLEEP); иконка циферблата
 *                показывается ТОЛЬКО пока идёт обратный отсчёт (AWAKE/
 *                PRESLEEP) — скрыта и в AWAKE, когда таймер не идёт
 *                (remaining==0), и в SLEEP ("Спит" показывается без
 *                иконки — инструмент уже спит, отсчитывать больше нечего),
 *                см. update_sleep_status()/s_sleep_icon_shown[]; по центру —
 *                сообщение EEPROM (Error_GetInfoZoneMessage()): транзитное
 *                ("сброшено на заводские", 5 сек) либо авария (весь
 *                сеанс); пусто, если показывать нечего.
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
 *    (Menu_IsShowingExpertWarning()), промта подтверждения сброса
 *    (Menu_IsShowingResetConfirm()) и сообщения о выполненном сбросе
 *    (Menu_IsShowingResetDone()) — отдельных полей под это не заведено.
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
#include "gfx.h"
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
#include <stdio.h>
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
/* Инфозона разбита на три поля по x: сообщение EEPROM по центру,
 * иконка+таймер сна паяльника — у ПРАВОГО края своей (левой) половины,
 * т.е. перед разделителем; иконка+таймер сна отсоса — у правого края
 * своей (правой) половины, т.е. у правого края экрана. Таймер всегда
 * выравнивается по правому краю (TextField_PrintfRightAligned), иконка —
 * ПЕРЕСЧИТЫВАЕТСЯ на каждое обновление по ФАКТИЧЕСКОЙ ширине текущего
 * текста таймера (см. update_sleep_status()), а не по одной статичной
 * позиции под "худший случай" — раньше расчёт брал самый широкий из
 * реальных текстов ("Предсон"/"99:59") и добавлял ручной сдвиг
 * SLEEP_ICON_X_OFFSET вправо, к более узкому обычному "M:SS", но это
 * означало, что при РЕАЛЬНОМ показе широкого текста ("Предсон") его
 * прямоугольник перекрывал область иконки и стирал её пикселями текста
 * (см. историю бага — "у второго таймера пропал циферблат"). Пересчёт по
 * факту на каждый вызов гарантированно исключает такое наложение — зазор
 * SLEEP_ICON_GAP_X между иконкой и текстом всегда одинаковый, независимо
 * от длины текста. Работает независимо для каждого инструмента (своя
 * половина экрана, свои координаты — см. вызовы update_sleep_status()).
 * Если сообщение EEPROM длинное — может визуально наехать на таймер
 * паяльника (он теперь ближе к центру, чем раньше); это приближённый
 * первый вариант, поправить координаты по факту на экране. */
#define SCREEN_INFO_EEPROM_X         (100U)
#define SCREEN_INFO_SLEEP_SOLDER_TEXT_RIGHT_EDGE_X   (SCREEN_DIVIDER_X0 - 6U)
#define SCREEN_INFO_SLEEP_DESOLDER_TEXT_RIGHT_EDGE_X (SCREEN_WIDTH - 10U)

/* ---- Иконка "циферблат" перед таймером сна (белая; x пересчитывается
 * динамически по факту текста в update_sleep_status(), см. выше — y и
 * размер статичны) ---- */
#define SLEEP_ICON_W     (14U)
#define SLEEP_ICON_H     (14U)
#define SLEEP_ICON_GAP_X (4U)  /* зазор между иконкой и текстом таймера */
#define SLEEP_ICON_Y     (SCREEN_INFO_Y + 2U) /* вертикально примерно по центру строки таймера (шрифт 18) */
#define COLOR_SLEEP_ICON DISPLAY_RGB565(255, 255, 255)

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

#define COLOR_SLEEP_AWAKE    COLOR_INFO                       /* белый — обычный обратный отсчёт до PRESLEEP */
#define COLOR_SLEEP_PRESLEEP DISPLAY_RGB565(255, 210, 0)      /* жёлтый */
#define COLOR_SLEEP_SLEEP    DISPLAY_RGB565(255, 40, 40)      /* красный */

#define COLOR_MENU_TITLE    DISPLAY_RGB565(255, 255, 255)
#define COLOR_MENU_NORMAL   DISPLAY_RGB565(180, 180, 180)
#define COLOR_MENU_CURSOR   DISPLAY_RGB565(255, 210, 0)  /* выбранный пункт, не редактируется */
#define COLOR_MENU_EDITING  DISPLAY_RGB565(255, 80, 80)  /* выбранный пункт, редактируется прямо сейчас */

static channel_id_t s_last_active_channel;
static bool s_last_fault[CHANNEL_COUNT]; /* чтобы перекрашивать title/current только при реальном изменении неисправности */
static screen_mode_t s_last_screen_mode; /* чтобы очищать экран только при реальной смене режима, не каждый кадр */
static sleep_mode_t s_last_sleep_mode[CHANNEL_COUNT]; /* чтобы перекрашивать таймер сна только при реальной смене режима */
static uint16_t s_sleep_icon_x[CHANNEL_COUNT]; /* x, по которому иконка РЕАЛЬНО сейчас нарисована на экране (актуален только пока s_sleep_icon_shown[ch]==true) — пересчитывается в update_sleep_status() */
static bool s_sleep_icon_shown[CHANNEL_COUNT]; /* сейчас ли иконка реально нарисована на экране (скрыта, когда таймер не отображается) */

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
 * @brief Битмап иконки "циферблат", 14x14, 1 бит/пиксель (MSB=левый пиксель
 *        строки) — окружность + часовая/минутная стрелки. Нарисован вручную
 *        (не из шрифта — шрифты проекта не содержат символ часов/циферблата,
 *        см. bdf2c_TFT.py/fonts.h).
 */
static const uint16_t s_sleep_icon_bitmap[SLEEP_ICON_H] = {
    0b00011111110000,
    0b00111000111000,
    0b01100000001100,
    0b11000010000110,
    0b11000010000110,
    0b10000010000010,
    0b10000011000011,
    0b10000011100010,
    0b11000000110110,
    0b11000000000110,
    0b01100000001100,
    0b00111000111000,
    0b00011111110000,
    0b00000000000000,
};

/**
 * @brief Нарисовать иконку циферблата (статично, блокирующе — как и
 *        draw_divider(): редкое событие, старт/смена режима экрана, не
 *        каждый кадр). Буфер на стеке — функция дожидается завершения DMA
 *        перед возвратом, буфер не переживёт функцию иначе.
 *
 * @return true, если реально нарисована (Display_SetWindow() успешен).
 *         false — окно не выставилось (например, DISPLAY_ERROR); ничего
 *         не нарисовано. Вызывающий код (update_sleep_status()) ОБЯЗАН
 *         проверять результат и не фиксировать s_sleep_icon_shown[]/
 *         s_sleep_icon_x[] как "нарисовано", если он false — иначе
 *         состояние разъезжается с реальным экраном НАВСЕГДА (следующий
 *         вызов решит, что иконка уже там, где её на самом деле нет, и
 *         не предпримет повторной попытки) — см. историю бага "у второго
 *         таймера пропал циферблат".
 */
static bool draw_sleep_icon(uint16_t x, uint16_t y)
{
    display_color_t buf[SLEEP_ICON_W * SLEEP_ICON_H];

    for (uint16_t row = 0; row < SLEEP_ICON_H; row++) {
        uint16_t bits = s_sleep_icon_bitmap[row];
        for (uint16_t col = 0; col < SLEEP_ICON_W; col++) {
            bool on = ((bits >> (SLEEP_ICON_W - 1U - col)) & 0x1U) != 0U;
            buf[(row * SLEEP_ICON_W) + col] = on ? COLOR_SLEEP_ICON : COLOR_BG;
        }
    }

    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SLEEP_ICON_W - 1U), (uint16_t)(y + SLEEP_ICON_H - 1U)) != DISPLAY_OK) {
        return false;
    }
    Display_WritePixelsDMA(buf, (uint32_t)SLEEP_ICON_W * SLEEP_ICON_H);
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Стереть иконку циферблата (залить фоном) — когда таймер не
 *        отображается (см. update_sleep_status()). Тот же блокирующий
 *        паттерн, что и draw_sleep_icon() — редкое событие (смена
 *        видимости), не каждый кадр.
 *
 * @return true, если реально стёрта — тот же контракт и та же причина,
 *         что и у draw_sleep_icon() (см. её докстринг).
 */
static bool erase_sleep_icon(uint16_t x, uint16_t y)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SLEEP_ICON_W - 1U), (uint16_t)(y + SLEEP_ICON_H - 1U)) != DISPLAY_OK) {
        return false;
    }
    Display_FillColorDMA(COLOR_BG, (uint32_t)SLEEP_ICON_W * SLEEP_ICON_H);
    while (Display_IsBusy()) { }
    return true;
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
        /* Иконки циферблата НЕ рисуем принудительно здесь — их видимость
         * зависит от того, отображается ли сейчас таймер (см.
         * update_sleep_status()). Экран уже очищен фоном, поэтому просто
         * сбрасываем "показана" в false, чтобы ближайший
         * update_sleep_status() перерисовал иконку заново, если она должна
         * быть видна (даже если режим сна не изменился). */
        s_sleep_icon_shown[CHANNEL_SOLDER] = false;
        s_sleep_icon_shown[CHANNEL_DESOLDER] = false;
    }
    TextField_InvalidateAll(); /* все строки (обоих экранов) забывают, что было на экране — перерисуются с нуля на чистом фоне */
}

void Screen_Init(void)
{
    draw_divider();

    TextField_ConfigureLine(LINE_INFO, SCREEN_INFO_EEPROM_X, SCREEN_INFO_Y,
                             &AntiquaB_16_uni, COLOR_INFO, COLOR_BG);
    TextField_ConfigureLine(LINE_INFO_SLEEP_SOLDER, SCREEN_INFO_SLEEP_SOLDER_TEXT_RIGHT_EDGE_X, SCREEN_INFO_Y,
                             &AntiquaB_18_uni, COLOR_SLEEP_AWAKE, COLOR_BG);
    TextField_ConfigureLine(LINE_INFO_SLEEP_DESOLDER, SCREEN_INFO_SLEEP_DESOLDER_TEXT_RIGHT_EDGE_X, SCREEN_INFO_Y,
                             &AntiquaB_18_uni, COLOR_SLEEP_AWAKE, COLOR_BG);
    /* x, переданный здесь для этих двух строк, — просто начальное значение,
     * реальная позиция пересчитывается по факту при первом же
     * TextField_PrintfRightAligned() в Screen_Update(), как и у CURRENT/TARGET/пресетов. */

    /* s_sleep_icon_x[]/s_sleep_icon_shown[] инициализировать здесь не нужно —
     * обе статические (нули по умолчанию), а x всё равно пересчитывается
     * заново на каждый вызов update_sleep_status(), прежде чем что-либо
     * рисовать; первый же вызов из Screen_Update() нарисует иконку по
     * месту, если нужно. */

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
        s_last_sleep_mode[ch] = SLEEP_MODE_AWAKE; /* Sleep_Init() тоже гарантирует AWAKE по умолчанию; совпадает с COLOR_SLEEP_AWAKE выше */
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
    bool enabled = State_IsEnabled(ch);

    if (fault_msg != NULL) {
        /* Текущая температура НЕ выводится вообще — на её месте сообщение
         * в отдельном поле (Comic_60_dig кириллицу не содержит) */
        TextField_PrintfCentered(line_current, center_x, "");
        TextField_PrintfCentered(line_fault_msg, center_x, "%s", fault_msg);
    } else if (!enabled) {
        /* Канал выключен коротким UP+DN (см. fsm.c) — число текущей
         * температуры показывать бессмысленно (нагрев не идёт, значение
         * не поддерживается). "--" выводим ЧЕРЕЗ ТО ЖЕ поле fault_msg
         * (AntiquaB_18_uni), что и текст неисправности выше — дефис у
         * этого шрифта есть (в отличие от Comic_60_dig, где рисуется
         * число), и путь уже проверен: скрыть number, показать текст на
         * том же месте. Приоритет у fault_msg (реальная неисправность
         * важнее статуса "выключено вручную"). */
        TextField_PrintfCentered(line_current, center_x, "");
        TextField_PrintfCentered(line_fault_msg, center_x, "--");
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
 * @brief Обновить поле таймера сна одного канала в инфозоне (текст, цвет
 *        И иконку циферблата)
 *
 * AWAKE (простаивает, до PRESLEEP)       -> "MM:SS" + иконка, белый
 * AWAKE (не простаивает / выключено)     -> "" (пусто), без иконки, белый
 * PRESLEEP (до SLEEP)                    -> "MM:SS" + иконка, жёлтый
 * PRESLEEP (SleepTimeout выключен)       -> "Предсон" + иконка (без времени — бессрочно), жёлтый
 * SLEEP                                   -> "Спит" + иконка (физического
 *                                            снижения нагрева при входе в
 *                                            SLEEP пока нет — см. ниже), красный
 *
 * Текст всегда выравнивается по правому краю right_edge_x. Иконка
 * циферблата ставится СЛЕВА от него вплотную (зазор SLEEP_ICON_GAP_X),
 * поэтому у любого по длине текста ("Спит"/"Предсон"/"M:SS"/"MM:SS")
 * иконка гарантированно не перекрывается текстом.
 *
 * Иконка (и позиция, и видимость) пересчитывается ТОЛЬКО когда строка
 * settled (TextField_IsSettled() — её text совпал с shown_text, т.е.
 * предыдущее задание рендера для неё точно завершилось). В этот момент
 * TextField_GetShownWidth() возвращает уже финальную, реально нарисованную
 * ширину — никакого "запаса"/максимума брать не нужно. Пока строка не
 * settled, иконка вообще не трогается и остаётся там, где её поставили на
 * предыдущем settled-состоянии — это безопасно по определению: рядом с ней
 * тогда лежал именно тот текст, который сейчас ещё физически на экране
 * (новый buf, посчитанный чуть выше, туда ещё не долетел). См. историю
 * бага и докстринг TextField_GetShownWidth()/TextField_IsSettled() в
 * text_field.h.
 */
static void update_sleep_status(channel_id_t ch, uint8_t line, uint16_t right_edge_x)
{
    bool enabled = State_IsEnabled(ch);
    sleep_mode_t mode = enabled ? Sleep_GetMode(ch) : SLEEP_MODE_AWAKE;
    uint32_t remaining = enabled ? Sleep_GetRemainingSeconds(ch) : 0;
    uint32_t min = remaining / 60U;
    uint32_t sec = remaining % 60U;
    bool timer_visible = true;
    char buf[16];

    /* Канал выключен коротким UP+DN — таймеру сна нечего отсчитывать
     * (физически не греет и так, "уснуть" ему не с чего): ведём себя как
     * AWAKE без простоя (mode/remaining уже принудительно приведены выше),
     * дальше switch отработает штатной веткой SLEEP_MODE_AWAKE/remaining==0
     * — пусто, без иконки. Sleep_GetMode()/Sleep_GetRemainingSeconds() не
     * зовём вовсе, когда выключено — модуль Sleep ничего не знает про
     * State_IsEnabled() и продолжает свой отсчёт по физическому простою
     * независимо от него, так что спрашивать его здесь бессмысленно. */

    switch (mode) {
        case SLEEP_MODE_AWAKE:
            if (remaining == 0) {
                buf[0] = '\0';
                timer_visible = false; /* нечего показывать — таймер не идёт */
            } else {
                snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)min, (unsigned long)sec);
            }
            break;
        case SLEEP_MODE_PRESLEEP:
            if (remaining == 0) snprintf(buf, sizeof(buf), "Предсон");
            else snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)min, (unsigned long)sec);
            break;
        case SLEEP_MODE_SLEEP:
            snprintf(buf, sizeof(buf), "Спит");
            /* timer_visible остаётся true: физического снижения нагрева при
             * входе в SLEEP пока не реализовано (Sleep_GetMode() нигде,
             * кроме отображения, не используется) — "инструмент уже спит"
             * пока не факт, а просто истёкший таймер. Прятать иконку по
             * этой причине преждевременно; вернуть вопрос, когда появится
             * реальная логика снижения нагрева по Presleep/Sleep-температуре. */
            break;
        default:
            buf[0] = '\0';
            break;
    }
    TextField_PrintfRightAligned(line, right_edge_x, "%s", buf);

    /* Трогаем иконку только когда строка settled — см. докстринг функции
     * выше и TextField_IsSettled() в text_field.h. Пока не settled (buf
     * только что запрошен, но предыдущее задание для этой строки ещё не
     * доиграно), просто ничего не делаем — иконка остаётся там, где её
     * оставило предыдущее settled-состояние, и это безопасно по
     * определению. */
    if (TextField_IsSettled(line)) {
        uint16_t new_icon_x = s_sleep_icon_x[ch];
        if (timer_visible) {
            /* Строка settled -> shown_text уже равен buf -> ширина финальная,
             * реально нарисованная. Никакого max() с шириной buf не нужно —
             * это одна и та же ширина. */
            uint16_t shown_w = TextField_GetShownWidth(line);
            new_icon_x = (uint16_t)(right_edge_x - shown_w - SLEEP_ICON_GAP_X - SLEEP_ICON_W);
        }
        bool icon_moved = timer_visible && s_sleep_icon_shown[ch] && (new_icon_x != s_sleep_icon_x[ch]);
        if (timer_visible != s_sleep_icon_shown[ch] || icon_moved) {
            /* Состояние (s_sleep_icon_shown[]/s_sleep_icon_x[]) фиксируем
             * ТОЛЬКО по факту успеха каждой операции — не "оптимистично".
             * Если стирание/рисование не удалось, оставляем состояние как
             * было (или как получилось после частичного успеха), чтобы
             * следующий settled-вызов сам повторил недостающий шаг, а не
             * решил, что экран уже соответствует желаемому виду. */
            if (s_sleep_icon_shown[ch]) {
                if (erase_sleep_icon(s_sleep_icon_x[ch], SLEEP_ICON_Y)) {
                    s_sleep_icon_shown[ch] = false; /* точно стёрта, старое место чистое */
                }
            }
            if (timer_visible && !s_sleep_icon_shown[ch]) {
                if (draw_sleep_icon(new_icon_x, SLEEP_ICON_Y)) {
                    s_sleep_icon_x[ch] = new_icon_x;
                    s_sleep_icon_shown[ch] = true;
                }
            }
        }
    }

    if (mode != s_last_sleep_mode[ch]) {
        display_color_t color;
        switch (mode) {
            case SLEEP_MODE_PRESLEEP: color = COLOR_SLEEP_PRESLEEP; break;
            case SLEEP_MODE_SLEEP:    color = COLOR_SLEEP_SLEEP;    break;
            default:                  color = COLOR_SLEEP_AWAKE;   break;
        }
        TextField_SetColors(line, color, COLOR_BG);
        s_last_sleep_mode[ch] = mode;
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

    if (Menu_IsShowingResetConfirm()) {
        for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
            uint8_t line = (uint8_t)(LINE_MENU_ITEM_0 + i);
            if (i < 3) {
                TextField_Printf(line, "%s", Menu_GetResetConfirmLine(i));
            } else {
                TextField_Printf(line, "");
            }
            TextField_SetColors(line, COLOR_MENU_NORMAL, COLOR_BG);
        }
        return;
    }

    if (Menu_IsShowingResetDone()) {
        for (uint8_t i = 0; i < SCREEN_MENU_ITEM_ROWS; i++) {
            uint8_t line = (uint8_t)(LINE_MENU_ITEM_0 + i);
            if (i < 3) {
                TextField_Printf(line, "%s", Menu_GetResetDoneLine(i));
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

    update_sleep_status(CHANNEL_SOLDER, LINE_INFO_SLEEP_SOLDER, SCREEN_INFO_SLEEP_SOLDER_TEXT_RIGHT_EDGE_X);
    update_sleep_status(CHANNEL_DESOLDER, LINE_INFO_SLEEP_DESOLDER, SCREEN_INFO_SLEEP_DESOLDER_TEXT_RIGHT_EDGE_X);
}
