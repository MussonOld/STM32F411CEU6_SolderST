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
 *                (remaining==0), и в SLEEP (там же пусто и без текста —
 *                иконка спящего смайлика на месте температуры уже
 *                достаточный сигнал, см. CHANNEL_CONTENT_ASLEEP; отдельная
 *                подпись "Спит" убрана, см. чат),
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
 *        сообщение об обрыве (LINE_x_FAULT_MSG/LINE_x_FAULT_MSG2, 2 строки —
 *        2 слова в 1 строку не помещаются, см. print_fault_message_2line()),
 *        шрифт AntiquaB_18_uni, НА ТОЙ ЖЕ позиции — это ДВА РАЗНЫХ поля
 *        (Comic_60_dig кириллицу физически не содержит, нельзя вывести
 *        текст тем же полем), но в любой момент содержимое имеет ровно
 *        одно из двух, второе пустое (см. update_channel_content()):
 *        обычный случай — число в CURRENT, FAULT_MSG/FAULT_MSG2 пусты;
 *        RTD/нагреватель оборван — CURRENT пуст, FAULT_MSG/FAULT_MSG2 —
 *        текст по словам ("Обрыв"/"RTD", "Обрыв"/"нагревателя", "КЗ"/"RTD")
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
 *    Те же 7 строк переиспользуются для строк предупреждения Expert
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
#include "control.h"
#include "sleep_icon.h"
#include "solder_icon.h"
#include "desolder_icon.h"
#include <stddef.h>
#include <stdio.h>
#include "fixed_point.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <string.h>

/* ---- Индексы строк TextField ---- */
enum {
    LINE_INFO = 0,
    LINE_SOLDER_TITLE,
    LINE_SOLDER_CURRENT,      /* число, шрифт Comic_60_dig — непусто в NORMAL и ASLEEP ("--") */
    LINE_SOLDER_FAULT_MSG,    /* 1-я строка "Обрыв"/"КЗ"/"ERR", шрифт AntiquaB_18_uni — непусто только в CHANNEL_CONTENT_FAULT */
    LINE_SOLDER_FAULT_MSG2,   /* 2-я строка ("RTD"/"нагревателя"/"AD1220") — см. print_fault_message_2line() */
    LINE_SOLDER_DISABLED_MSG, /* "ВЫКЛ", шрифт AntiquaB_32_uni — непусто только в CHANNEL_CONTENT_DISABLED */
    LINE_SOLDER_TARGET,
    LINE_DESOLDER_TITLE,
    LINE_DESOLDER_CURRENT,
    LINE_DESOLDER_FAULT_MSG,
    LINE_DESOLDER_FAULT_MSG2,
    LINE_DESOLDER_DISABLED_MSG,
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
/* Раньше — высота шрифта AntiquaB_18_uni (текстовые заголовки "Паяльник"/
 * "Отсос"). Теперь оба заголовка — иконки (см. чат, solder_icon.h/
 * desolder_icon.h), взята высота более высокой из двух (Desolder, 31px
 * после ужатия до 120px по ширине), чтобы CURRENT-блок начинался на одном
 * и том же Y в обеих половинах экрана — иконка Solder (17px) центрируется
 * по вертикали в той же полосе, не растягиваясь. */
#define SCREEN_TITLE_HEIGHT ((DESOLDER_ICON_BITMAP_H > SOLDER_ICON_BITMAP_H) ? DESOLDER_ICON_BITMAP_H : SOLDER_ICON_BITMAP_H)

/* Центры половин экрана — используются TextField_PrintfCentered() для
 * температур, реальная ширина текста меряется на лету (не типовая). */
#define SCREEN_HALF_CENTER_LEFT_X   ((SCREEN_DIVIDER_X0) / 2U)
#define SCREEN_HALF_CENTER_RIGHT_X  (SCREEN_DIVIDER_X1 + 1U + SCREEN_HALF_CENTER_LEFT_X)

/* Иконки заголовков (растры SolderIcon_Bitmap/DesolderIcon_Bitmap, см.
 * solder_icon.h/desolder_icon.h) вместо текста "Паяльник"/"Отсос" (см. чат)
 * — центрируются по X тем же center_x, что и CURRENT/TARGET своей половины,
 * и по Y — внутри общей титульной полосы (SCREEN_TITLE_HEIGHT, взята по
 * более высокой из двух иконок), а не по общему верхнему краю: иконки
 * разной высоты (17 и 31px после ужатия до 120px по ширине, см. чат) иначе
 * не совпадали бы по центру, хоть и совпадали бы по верху. */
#define SCREEN_SOLDER_ICON_X   ((uint16_t)(SCREEN_HALF_CENTER_LEFT_X  - SOLDER_ICON_BITMAP_W   / 2U))
#define SCREEN_DESOLDER_ICON_X ((uint16_t)(SCREEN_HALF_CENTER_RIGHT_X - DESOLDER_ICON_BITMAP_W / 2U))
#define SCREEN_SOLDER_ICON_Y   ((uint16_t)(SCREEN_TITLE_Y + (SCREEN_TITLE_HEIGHT - SOLDER_ICON_BITMAP_H)   / 2U))
#define SCREEN_DESOLDER_ICON_Y ((uint16_t)(SCREEN_TITLE_Y + (SCREEN_TITLE_HEIGHT - DESOLDER_ICON_BITMAP_H) / 2U))

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

/* Аварийное сообщение всегда в 2 строки (2 слова в одну не помещаются, см.
 * чат) — FAULT_MSG остаётся на позиции SCREEN_CURRENT_Y (как и раньше),
 * FAULT_MSG2 — сразу под ней. SCREEN_TITLE_HEIGHT — высота шрифта
 * AntiquaB_18_uni (тем же шрифтом выводятся обе строки), небольшой зазор
 * между ними. До SCREEN_TARGET_Y ещё много места (CURRENT-блок посчитан
 * под 67px строку Comic_60_dig, а тут всего 2×18px) — не пересекается. */
#define SCREEN_FAULT_MSG2_GAP (2U)
#define SCREEN_FAULT_MSG2_Y ((uint16_t)(SCREEN_CURRENT_Y + SCREEN_TITLE_HEIGHT + SCREEN_FAULT_MSG2_GAP))

/* "ВЫКЛ" (CHANNEL_CONTENT_DISABLED) — одна строка AntiquaB_32_uni,
 * вертикально по центру той же 67px CURRENT-полосы (см. чат — ВРЕМЕННО,
 * поправить по факту на экране: реальная высота глифов шрифта может не
 * совпадать с номиналом 32px из его имени). */
#define SCREEN_DISABLED_HEIGHT (32U)
#define SCREEN_DISABLED_MSG_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SCREEN_DISABLED_HEIGHT) / 2U))

/* Крестик "инструмент не подключен" (CHANNEL_CONTENT_IDLE, см.
 * draw_idle_cross()) — растр, вычисляется на лету (не хранится константой,
 * в отличие от SLEEP_ICON_BITMAP — тут это просто тест "около диагонали",
 * ручками эту сетку не набирали). Квадрат, вписанный в CURRENT-полосу
 * (67px), с запасом по бокам для обеих половин экрана (~79px). ВРЕМЕННО —
 * поправить размер/толщину по факту на экране. */
#define SCREEN_IDLE_CROSS_SIZE      (48U)
#define SCREEN_IDLE_CROSS_THICKNESS (4U)  /* полутолщина луча, см. draw_idle_cross() */
#define SCREEN_IDLE_CROSS_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SCREEN_IDLE_CROSS_SIZE) / 2U))
#define COLOR_IDLE_CROSS DISPLAY_RGB565(120, 120, 120) /* см. чат — "пониженная контрастность, как и сейчас" (тусклее обычного текста) */

/* Иконка SLEEP (спящий смайлик, растр SleepIcon_Bitmap — см. sleep_icon.h)
 * вместо "--" в CHANNEL_CONTENT_ASLEEP (см. чат). Тот же принцип
 * центрирования по Y, что и у крестика IDLE (вписана в 67px CURRENT-полосу,
 * своя высота из sleep_icon.h вместо SCREEN_IDLE_CROSS_SIZE); по X —
 * центрируется на center_x своей половины экрана, как и крестик. */
#define SCREEN_ASLEEP_ICON_Y ((uint16_t)(SCREEN_CURRENT_Y + (SCREEN_CURRENT_HEIGHT - SLEEP_ICON_BITMAP_H) / 2U))

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
 * Сообщение EEPROM выводится от самого левого края (SCREEN_INFO_X), а не
 * от центра — см. чат: на SCREEN_INFO_EEPROM_X=100 оно визуально наезжало
 * на таймер паяльника (см. TextField_ConfigureLine(LINE_INFO, ...) ниже). */
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

/**
 * @brief Взаимоисключающие состояния содержимого CURRENT-блока канала
 *        (см. update_channel_content()) — раньше отслеживался только один
 *        bool (авария/не авария), теперь состояний пять, и переключение
 *        между ЛЮБОЙ парой требует того же гашения-и-ожидания-settled, что
 *        раньше делалось только для аварии (см. s_content_clearing ниже):
 *        каждое состояние рисует CURRENT-блок по-своему (число, "--",
 *        2-строчный текст аварии, "ВЫКЛ" отдельным полем, крестик отдельным
 *        растром — см. ADS1220_SETTLE ниже про растр), и они физически
 *        делят одну Y-полосу.
 */
typedef enum {
    CHANNEL_CONTENT_NORMAL = 0, /* число текущей температуры (Comic_60_dig) */
    CHANNEL_CONTENT_FAULT,      /* авария — 2-строчное сообщение (AntiquaB_18_uni) */
    CHANNEL_CONTENT_IDLE,       /* инструмент не подключен — растровый крестик, без текста */
    CHANNEL_CONTENT_DISABLED,   /* канал выключен аккордом — "ВЫКЛ" (AntiquaB_32_uni) */
    CHANNEL_CONTENT_ASLEEP,     /* SLEEP_MODE_SLEEP — растровая иконка (спящий смайлик, см. sleep_icon.h), раньше здесь было "--" */
} channel_content_t;

static channel_content_t s_last_content[CHANNEL_COUNT]; /* чтобы перекрашивать title/current и гасить блок только при реальной смене состояния */
static bool s_content_clearing[CHANNEL_COUNT]; /* см. update_channel_content() — гасим CURRENT/FAULT_MSG/FAULT_MSG2/DISABLED_MSG
                                                 * (и крестик, отдельно, см. s_idle_icon_shown) перед сменой состояния, чтобы
                                                 * стирание одного поля не затёрло уже нарисованное содержимое другого (они
                                                 * физически перекрываются по Y, см. докстринг файла) */
static bool s_idle_icon_shown[CHANNEL_COUNT]; /* крестик "инструмент не подключен" сейчас реально нарисован на экране (растр, см. draw_idle_cross()) —
                                                * рисуется/стирается сырым Display_WritePixelsDMA в обход TextField, поэтому
                                                * TextField не знает про эти пиксели и не сотрёт их сам при смене состояния (см. update_channel_content()) */
static bool s_asleep_icon_shown[CHANNEL_COUNT]; /* иконка SLEEP (спящий смайлик) сейчас реально нарисована — тот же смысл
                                                  * и та же причина, что и у s_idle_icon_shown (см. draw_asleep_icon()) */
static screen_mode_t s_last_screen_mode; /* чтобы очищать экран только при реальной смене режима, не каждый кадр */
static display_color_t s_last_sleep_color[CHANNEL_COUNT]; /* чтобы перекрашивать таймер сна только при реальной смене цвета (не режима — один и тот же mode может значить разный цвет, см. update_sleep_status()) */
static uint16_t s_sleep_icon_x[CHANNEL_COUNT]; /* x, по которому иконка РЕАЛЬНО сейчас нарисована на экране (актуален только пока s_sleep_icon_shown[ch]==true) — пересчитывается в update_sleep_status() */
static bool s_sleep_icon_shown[CHANNEL_COUNT]; /* сейчас ли иконка реально нарисована на экране (скрыта, когда таймер не отображается) */
static int32_t s_temp_shown[CHANNEL_COUNT];      /* целое, которое сейчас показывает CURRENT (актуально только при s_temp_shown_valid) */
static bool    s_temp_shown_valid[CHANNEL_COUNT]; /* false, пока CURRENT показывает не число ("--"/авария) — следующее число берётся без гистерезиса */

/**
 * @brief Гистерезис вывода текущей температуры, °C.
 *
 * Отображаемое целое S соответствует реальной t из [S, S+1) (FIXED_TO_INT —
 * арифметический сдвиг, т.е. floor). Число меняется только когда t выходит
 * из [S - H, S + 1 + H): на границе двух целых медленный дрейф/шум АЦП больше
 * не даёт мельтешения S <-> S+1. Только для вывода — Control работает с
 * нефильтрованной температурой из State.
 */
#define SCREEN_TEMP_HYST_C  FIXED_FROM_FLOAT(0.3f)

/**
 * @brief Применить цвета title/current канала. Приоритет: неисправность
 *        (красный) > активный/неактивный. Target сюда не входит — он
 *        всегда числом и обычной активной/неактивной окраской (ВРЕМЕННО,
 *        см. докстринг файла). Пресеты тоже не входят — общие поля.
 */
static void apply_channel_colors(channel_id_t ch)
{
    uint8_t line_title, line_current, line_disabled_msg;
    bool active = (ch == InputFSM_GetActiveChannel());
    bool faulted = Error_IsChannelFaulted(ch);

    if (ch == CHANNEL_SOLDER) {
        line_title        = LINE_SOLDER_TITLE;
        line_current      = LINE_SOLDER_CURRENT;
        line_disabled_msg = LINE_SOLDER_DISABLED_MSG;
    } else {
        line_title        = LINE_DESOLDER_TITLE;
        line_current      = LINE_DESOLDER_CURRENT;
        line_disabled_msg = LINE_DESOLDER_DISABLED_MSG;
    }

    if (faulted) {
        TextField_SetColors(line_title,   COLOR_FAULT, COLOR_BG); /* заголовок теперь иконка (см. draw_solder_icon()/draw_desolder_icon()), эта строка ничего не красит на экране — no-op, оставлено чтобы не усложнять функцию веткой на канал */
        TextField_SetColors(line_current, COLOR_FAULT, COLOR_BG);
    } else {
        TextField_SetColors(line_title,   active ? COLOR_ACTIVE_TITLE   : COLOR_INACTIVE_TITLE,   COLOR_BG); /* тот же no-op, что и выше */
        TextField_SetColors(line_current, active ? COLOR_ACTIVE_CURRENT : COLOR_INACTIVE_CURRENT, COLOR_BG);
        /* CHANNEL_CONTENT_DISABLED ("ВЫКЛ") — та же активная/неактивная
         * пара, что и у CURRENT (авария и disabled взаимоисключающие, см.
         * update_channel_content(), так что faulted-ветку сюда заводить не нужно). */
        TextField_SetColors(line_disabled_msg, active ? COLOR_ACTIVE_CURRENT : COLOR_INACTIVE_CURRENT, COLOR_BG);
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
 * @brief Крестик "инструмент не подключен" (CHANNEL_CONTENT_IDLE) —
 *        квадрат SCREEN_IDLE_CROSS_SIZE, две диагонали толщиной
 *        2*SCREEN_IDLE_CROSS_THICKNESS+1. В отличие от циферблата (см.
 *        s_sleep_icon_bitmap), тут нет готовой битовой сетки — тест "у
 *        диагонали" считается построчно, в СТАТИЧЕСКИЙ (не стековый) буфер
 *        на одну строку: полный буфер 48x48 пикселей (4.6 КБ) на стеке — в
 *        одном кадре с main()/Screen_Update() и без RTOS многовато (см. чат
 *        про IWDG/размер стека), одна строка (96 байт) безопасна.
 *        Блокирующий построчный вывод — редкое событие (смена
 *        видимости), не каждый кадр, как и у циферблата.
 */
static bool draw_idle_cross(uint16_t x, uint16_t y)
{
    static display_color_t s_row_buf[SCREEN_IDLE_CROSS_SIZE]; /* см. докстринг — не на стеке */
    const uint16_t size = SCREEN_IDLE_CROSS_SIZE;
    const uint16_t t = SCREEN_IDLE_CROSS_THICKNESS;

    while (Display_IsBusy()) { }
    for (uint16_t row = 0; row < size; row++) {
        for (uint16_t col = 0; col < size; col++) {
            uint16_t anti_col = (uint16_t)(size - 1U - col);
            bool on_main_diag = (uint16_t)((row > col) ? (row - col) : (col - row)) <= t;
            bool on_anti_diag = (uint16_t)((row > anti_col) ? (row - anti_col) : (anti_col - row)) <= t;
            s_row_buf[col] = (on_main_diag || on_anti_diag) ? COLOR_IDLE_CROSS : COLOR_BG;
        }
        if (Display_SetWindow(x, (uint16_t)(y + row), (uint16_t)(x + size - 1U), (uint16_t)(y + row)) != DISPLAY_OK) {
            return false;
        }
        Display_WritePixelsDMA(s_row_buf, size);
        while (Display_IsBusy()) { }
    }
    return true;
}

/** @brief Стереть крестик (залить фоном) — тот же контракт, что и erase_sleep_icon(). */
static bool erase_idle_cross(uint16_t x, uint16_t y)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SCREEN_IDLE_CROSS_SIZE - 1U),
                           (uint16_t)(y + SCREEN_IDLE_CROSS_SIZE - 1U)) != DISPLAY_OK) {
        return false;
    }
    Display_FillColorDMA(COLOR_BG, (uint32_t)SCREEN_IDLE_CROSS_SIZE * SCREEN_IDLE_CROSS_SIZE);
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Нарисовать иконку SLEEP (спящий смайлик, готовый растр
 *        SleepIcon_Bitmap из sleep_icon.h/.c — RGB565, сгенерирован из PNG,
 *        см. чат). Тот же блокирующий паттерн, что и draw_sleep_icon()/
 *        draw_idle_cross() (редкое событие — вход в SLEEP, не каждый кадр).
 *        В отличие от них — данные уже готовы построчно в Flash, буфер (ни
 *        стековый, ни статический) не нужен, пишем прямо из константного
 *        массива одним DMA-проходом.
 *
 * @return true, если реально нарисована — тот же контракт, что и у
 *         draw_sleep_icon()/draw_idle_cross() (см. их докстринги про
 *         s_asleep_icon_shown ниже).
 */
static bool draw_asleep_icon(uint16_t x, uint16_t y)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SLEEP_ICON_BITMAP_W - 1U),
                           (uint16_t)(y + SLEEP_ICON_BITMAP_H - 1U)) != DISPLAY_OK) {
        return false;
    }
    Display_WritePixelsDMA(SleepIcon_Bitmap, (uint32_t)SLEEP_ICON_BITMAP_W * SLEEP_ICON_BITMAP_H);
    while (Display_IsBusy()) { }
    return true;
}

/** @brief Стереть иконку SLEEP (залить фоном) — тот же контракт, что и erase_idle_cross(). */
static bool erase_asleep_icon(uint16_t x, uint16_t y)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SLEEP_ICON_BITMAP_W - 1U),
                           (uint16_t)(y + SLEEP_ICON_BITMAP_H - 1U)) != DISPLAY_OK) {
        return false;
    }
    Display_FillColorDMA(COLOR_BG, (uint32_t)SLEEP_ICON_BITMAP_W * SLEEP_ICON_BITMAP_H);
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Нарисовать иконки заголовков ("Паяльник"/"Отсос", см.
 *        solder_icon.h/desolder_icon.h) — статичны, без erase-пары: в
 *        отличие от иконок IDLE/SLEEP (зависят от состояния канала и
 *        стираются/перерисовываются по ходу работы), эти рисуются только
 *        два раза за всё время — в Screen_Init() и повторно в
 *        clear_screen_for_mode_switch() после полной заливки экрана при
 *        возврате из сервисного меню (см. её докстринг). Тот же блокирующий
 *        паттерн одним DMA-проходом, что и draw_asleep_icon().
 */
static void draw_solder_icon(void)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(SCREEN_SOLDER_ICON_X, SCREEN_SOLDER_ICON_Y,
                           (uint16_t)(SCREEN_SOLDER_ICON_X + SOLDER_ICON_BITMAP_W - 1U),
                           (uint16_t)(SCREEN_SOLDER_ICON_Y + SOLDER_ICON_BITMAP_H - 1U)) != DISPLAY_OK) {
        return;
    }
    Display_WritePixelsDMA(SolderIcon_Bitmap, (uint32_t)SOLDER_ICON_BITMAP_W * SOLDER_ICON_BITMAP_H);
    while (Display_IsBusy()) { }
}

/** @brief Нарисовать иконку заголовка "Отсос" — тот же контракт, что и draw_solder_icon(). */
static void draw_desolder_icon(void)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(SCREEN_DESOLDER_ICON_X, SCREEN_DESOLDER_ICON_Y,
                           (uint16_t)(SCREEN_DESOLDER_ICON_X + DESOLDER_ICON_BITMAP_W - 1U),
                           (uint16_t)(SCREEN_DESOLDER_ICON_Y + DESOLDER_ICON_BITMAP_H - 1U)) != DISPLAY_OK) {
        return;
    }
    Display_WritePixelsDMA(DesolderIcon_Bitmap, (uint32_t)DESOLDER_ICON_BITMAP_W * DESOLDER_ICON_BITMAP_H);
    while (Display_IsBusy()) { }
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
        /* Иконки циферблата и крестика НЕ рисуем принудительно здесь — их
         * видимость зависит от текущего состояния (см.
         * update_sleep_status()/update_channel_content()). Экран уже
         * очищен фоном, поэтому просто сбрасываем "показана" в false: сами
         * растры рисуются сырым DMA в обход TextField, и TextField_InvalidateAll()
         * ниже про них не знает — не сбросив это здесь, ближайший
         * update_sleep_status()/update_channel_content() решил бы, что
         * иконка/крестик уже на экране (raз "показана"=true с прошлого
         * раза), и не перерисовал бы их на самом деле чистом фоне (см.
         * чат — крестик не подключенного канала не появлялся после выхода
         * из меню). */
        s_sleep_icon_shown[CHANNEL_SOLDER] = false;
        s_sleep_icon_shown[CHANNEL_DESOLDER] = false;
        s_idle_icon_shown[CHANNEL_SOLDER] = false;
        s_idle_icon_shown[CHANNEL_DESOLDER] = false;
        s_asleep_icon_shown[CHANNEL_SOLDER] = false;
        s_asleep_icon_shown[CHANNEL_DESOLDER] = false;
    }
    TextField_InvalidateAll(); /* все строки (обоих экранов) забывают, что было на экране — перерисуются с нуля на чистом фоне */

    if (new_mode == SCREEN_MODE_MAIN) {
        /* Иконки заголовков ("Паяльник"/"Отсос") рисуются raw DMA один раз в
         * Screen_Init() — Screen_Update() дальше меняет только ЦВЕТ подсветки
         * канала (apply_channel_colors -> TextField_SetColors на
         * LINE_*_TITLE, сейчас no-op — см. её комментарий), самих пикселей
         * иконки это не касается. Полная заливка фона выше стирает и их
         * тоже (растр в обход TextField, TextField_InvalidateAll() ниже про
         * эти пиксели не знает и перерисовать некому) — без вызова здесь
         * заголовки исчезают насовсем после первого же переключения режима
         * экрана (сервисное меню туда и обратно), ровно как раньше было бы
         * с текстом. */
        draw_solder_icon();
        draw_desolder_icon();
    }
}

void Screen_Init(void)
{
    draw_divider();

    TextField_ConfigureLine(LINE_INFO, SCREEN_INFO_X, SCREEN_INFO_Y,
                             &AntiquaB_18_uni, COLOR_INFO, COLOR_BG);
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

    /* LINE_SOLDER_TITLE больше не печатает текст (заголовок теперь иконка,
     * см. draw_solder_icon()/apply_channel_colors()) — TextField-строка
     * оставлена сконфигурированной просто чтобы TextField_SetColors() в
     * apply_channel_colors() (сейчас безвредный no-op на пустой строке)
     * не трогала неинициализированную линию; x/y и шрифт значения не имеют. */
    TextField_ConfigureLine(LINE_SOLDER_TITLE, SCREEN_HALF_CENTER_LEFT_X, SCREEN_TITLE_Y,
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
    TextField_ConfigureLine(LINE_SOLDER_FAULT_MSG2, SCREEN_HALF_CENTER_LEFT_X, SCREEN_FAULT_MSG2_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_DISABLED_MSG, SCREEN_HALF_CENTER_LEFT_X, SCREEN_DISABLED_MSG_Y,
                             &AntiquaB_32_uni, COLOR_ACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_TARGET, SCREEN_HALF_CENTER_LEFT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_ACTIVE_TARGET, COLOR_BG);

    /* LINE_DESOLDER_TITLE — тот же no-op, что и у LINE_SOLDER_TITLE выше. */
    TextField_ConfigureLine(LINE_DESOLDER_TITLE, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_INACTIVE_TITLE, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_CURRENT, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_CURRENT_Y,
                             &Comic_60_dig, COLOR_INACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_FAULT_MSG, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_CURRENT_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_FAULT_MSG2, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_FAULT_MSG2_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_DISABLED_MSG, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_DISABLED_MSG_Y,
                             &AntiquaB_32_uni, COLOR_INACTIVE_CURRENT, COLOR_BG);
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

    /* Заголовки каналов — иконки (растр, раз и навсегда, см. draw_solder_icon()/
     * draw_desolder_icon()), не текст. */
    draw_solder_icon();
    draw_desolder_icon();

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
        s_last_content[ch] = CHANNEL_CONTENT_NORMAL; /* Error_Init()/State_Init() тоже гарантируют "нет неисправности"/enabled по умолчанию */
        s_idle_icon_shown[ch] = false;
        s_last_sleep_color[ch] = COLOR_SLEEP_AWAKE; /* Sleep_Init() тоже гарантирует AWAKE/remaining=0 по умолчанию -> пусто, цвет не виден, но белый — нейтральный старт */
    }
    /* Solder активен по умолчанию при старте (см. InputFSM_Init()) —
     * начальные цвета выше уже расставлены соответственно. */
    s_last_active_channel = CHANNEL_SOLDER;
    s_last_screen_mode = SCREEN_MODE_MAIN; /* совпадает с InputFSM_Init() — при первом Screen_Update() лишней очистки не будет */
}

/**
 * @brief Аварийное сообщение всегда в 2 строки — 2 слова в одну строку не
 *        помещаются (см. чат). Делит msg по первому пробелу: "Обрыв
 *        нагревателя" -> "Обрыв"/"нагревателя", "КЗ RTD" -> "КЗ"/"RTD".
 *        Однословных сообщений сейчас нет (см. error.c), но на случай
 *        появления — целиком в line1, line2 пустая.
 */
static void print_fault_message_2line(uint8_t line1, uint8_t line2, uint16_t center_x, const char *msg)
{
    const char *space = strchr(msg, ' ');
    if (space != NULL) {
        TextField_PrintfCentered(line1, center_x, "%.*s", (int)(space - msg), msg);
        TextField_PrintfCentered(line2, center_x, "%s", space + 1);
    } else {
        TextField_PrintfCentered(line1, center_x, "%s", msg);
        TextField_PrintfCentered(line2, center_x, "");
    }
}

/**
 * @brief Обновить содержимое канала: title-цвет, current/fault_msg (ровно
 *        одно из двух непусто), target (всегда число)
 * @param center_x Центр половины экрана этого канала
 */
/**
 * @brief Целое для вывода в CURRENT с гистерезисом (см. SCREEN_TEMP_HYST_C).
 */
static int32_t temp_for_display(channel_id_t ch, fixed_t cur)
{
    if (s_temp_shown_valid[ch]) {
        fixed_t lo = FIXED_FROM_INT(s_temp_shown[ch]) - SCREEN_TEMP_HYST_C;
        fixed_t hi = FIXED_FROM_INT(s_temp_shown[ch] + 1) + SCREEN_TEMP_HYST_C;
        if (cur >= lo && cur < hi) {
            return s_temp_shown[ch]; /* в пределах гистерезиса — не меняем */
        }
    }
    s_temp_shown[ch] = FIXED_TO_INT(cur);
    s_temp_shown_valid[ch] = true;
    return s_temp_shown[ch];
}

bool Screen_GetShownTemp(channel_id_t ch, int32_t *out_temp)
{
    if (!s_temp_shown_valid[ch]) {
        return false;
    }
    *out_temp = s_temp_shown[ch];
    return true;
}

static void update_channel_content(channel_id_t ch, uint16_t center_x)
{
    uint8_t line_current      = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_CURRENT      : LINE_DESOLDER_CURRENT;
    uint8_t line_fault_msg    = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_FAULT_MSG    : LINE_DESOLDER_FAULT_MSG;
    uint8_t line_fault_msg2   = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_FAULT_MSG2   : LINE_DESOLDER_FAULT_MSG2;
    uint8_t line_disabled_msg = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_DISABLED_MSG : LINE_DESOLDER_DISABLED_MSG;
    uint8_t line_target       = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_TARGET      : LINE_DESOLDER_TARGET;
    uint16_t cross_x = (uint16_t)(center_x - SCREEN_IDLE_CROSS_SIZE / 2U);
    uint16_t asleep_icon_x = (uint16_t)(center_x - SLEEP_ICON_BITMAP_W / 2U);

    const char *fault_msg = Error_GetChannelFaultMessage(ch); /* не NULL только для аварий: RTD_SHORT/RTD_OPEN/HEATER_OPEN/ERR AD1220 */
    bool enabled = State_IsEnabled(ch);
    bool idle    = Error_IsChannelIdle(ch); /* инструмент не подключен — не авария, крестик (см. draw_idle_cross()) */
    bool faulted = Error_IsChannelFaulted(ch); /* нужно для перекраски title/current в конце функции, см. apply_channel_colors() */

    /* Приоритет состояний (см. чат): авария > idle > выключен аккордом >
     * спит (SLEEP_MODE_SLEEP) > обычное число. idle/fault физически не
     * пересекаются (Error_GetToolFault() — одно значение на канал, см.
     * error.h), остальные пары — независимые подсистемы (State/Sleep),
     * поэтому порядок ниже важен. */
    channel_content_t content;
    if (fault_msg != NULL)      content = CHANNEL_CONTENT_FAULT;
    else if (idle)              content = CHANNEL_CONTENT_IDLE;
    else if (!enabled)          content = CHANNEL_CONTENT_DISABLED;
    else if (Sleep_GetMode(ch) == SLEEP_MODE_SLEEP) content = CHANNEL_CONTENT_ASLEEP;
    else                        content = CHANNEL_CONTENT_NORMAL;

    /* CURRENT (Comic_60_dig), FAULT_MSG/FAULT_MSG2 (AntiquaB_18_uni),
     * DISABLED_MSG (AntiquaB_32_uni) и крестик (растр, см. draw_idle_cross())
     * физически делят одну и ту же Y-полосу (см. докстринг файла) — в любой
     * момент непусто ровно одно из них. TextField_Process() отрисовывает по
     * одной dirty-строке за вызов, начиная с САМОГО МЕЛКОГО индекса (см.
     * text_field.c) — у CURRENT индекс меньше, чем у FAULT_MSG/2/DISABLED_MSG.
     * Если писать новое содержимое напрямую на кадре смены состояния, при
     * ПОЯВЛЕНИИ более позднего по индексу поля порядок безопасен (CURRENT
     * первым гасится на "", остальные рисуют текст на уже пустом месте), а
     * вот при возврате К CURRENT порядок ломается: CURRENT (меньший индекс)
     * рисуется первым и получает число/"--", а прежнее поле гасится только
     * ПОСЛЕ — и его стирание старого текста (та же Y-полоса!) затирает уже
     * нарисованное. Статичным порядком индексов это не решить (для
     * противоположного перехода порядок снова стал бы неверным) — поэтому на
     * самом переходе сначала гасим ВСЕ ТЕКСТОВЫЕ поля и ждём, пока реально
     * доиграет отрисовка (TextField_IsSettled()), и только потом на
     * следующих вызовах рисуем настоящее новое содержимое (см. чат —
     * "на месте записи остаётся незаполненное пространство").
     *
     * Крестик рисуется СЫРЫМ Display_WritePixelsDMA в обход TextField
     * (см. s_idle_icon_shown) — TextField не знает про эти пиксели и не
     * сотрёт их сам ни на выходе из IDLE, ни держа старое "--"/число под
     * ним, поэтому erase_idle_cross() вызывается явно на выходе из IDLE,
     * ДО того как эта фаза гашения текстовых полей вообще запускается —
     * иначе крестик остаётся видимым поверх нового текста ещё один кадр. */
    if (s_last_content[ch] == CHANNEL_CONTENT_IDLE && content != CHANNEL_CONTENT_IDLE && s_idle_icon_shown[ch]) {
        if (erase_idle_cross(cross_x, SCREEN_IDLE_CROSS_Y)) {
            s_idle_icon_shown[ch] = false;
        }
    }
    /* Иконка SLEEP — та же логика и та же причина (растр в обход TextField,
     * см. докстринг выше про s_idle_icon_shown): гасим её явно на выходе из
     * ASLEEP, до фазы гашения текстовых полей ниже. */
    if (s_last_content[ch] == CHANNEL_CONTENT_ASLEEP && content != CHANNEL_CONTENT_ASLEEP && s_asleep_icon_shown[ch]) {
        if (erase_asleep_icon(asleep_icon_x, SCREEN_ASLEEP_ICON_Y)) {
            s_asleep_icon_shown[ch] = false;
        }
    }

    if (!s_content_clearing[ch] && content != s_last_content[ch]) {
        TextField_PrintfCentered(line_current, center_x, "");
        TextField_PrintfCentered(line_fault_msg, center_x, "");
        TextField_PrintfCentered(line_fault_msg2, center_x, "");
        TextField_PrintfCentered(line_disabled_msg, center_x, "");
        s_content_clearing[ch] = true;
    }

    if (s_content_clearing[ch]) {
        if (TextField_IsSettled(line_current) && TextField_IsSettled(line_fault_msg)
            && TextField_IsSettled(line_fault_msg2) && TextField_IsSettled(line_disabled_msg)) {
            s_content_clearing[ch] = false;
        }
    }

    if (!s_content_clearing[ch]) {
        if (content != CHANNEL_CONTENT_NORMAL) {
            s_temp_shown_valid[ch] = false; /* число не показываем — при возврате начинаем без гистерезиса */
        }
        switch (content) {
            case CHANNEL_CONTENT_FAULT:
                /* Текущая температура НЕ выводится вообще — на её месте сообщение
                 * в отдельном поле (Comic_60_dig кириллицу не содержит), в 2 строки. */
                print_fault_message_2line(line_fault_msg, line_fault_msg2, center_x, fault_msg);
                break;
            case CHANNEL_CONTENT_IDLE:
                /* Инструмент не подключен: ни красного, ни зуммера (см.
                 * error.h) — растровый крестик пониженной контрастности
                 * (см. COLOR_IDLE_CROSS) вместо текста. */
                if (!s_idle_icon_shown[ch]) {
                    if (draw_idle_cross(cross_x, SCREEN_IDLE_CROSS_Y)) {
                        s_idle_icon_shown[ch] = true;
                    }
                }
                break;
            case CHANNEL_CONTENT_DISABLED:
                /* Канал выключен коротким UP+DN (см. fsm.c) — "ВЫКЛ"
                 * отдельным полем AntiquaB_32_uni (Comic_60_dig кириллицу не
                 * содержит, как и у сообщений аварии). */
                TextField_PrintfCentered(line_disabled_msg, center_x, "ВЫКЛ");
                break;
            case CHANNEL_CONTENT_ASLEEP:
                /* SLEEP_MODE_SLEEP — вместо "--" растровая иконка (спящий
                 * смайлик, см. sleep_icon.h/чат), сырым Display_WritePixelsDMA
                 * в обход TextField — тот же паттерн, что и у крестика IDLE
                 * (см. draw_idle_cross()/s_idle_icon_shown). Реального
                 * снижения нагрева при входе в SLEEP по-прежнему нет (см.
                 * Sleep.md) — иконка тут чисто индикация состояния. */
                if (!s_asleep_icon_shown[ch]) {
                    if (draw_asleep_icon(asleep_icon_x, SCREEN_ASLEEP_ICON_Y)) {
                        s_asleep_icon_shown[ch] = true;
                    }
                }
                break;
            case CHANNEL_CONTENT_NORMAL:
            default: {
                fixed_t cur = State_GetCurrentTemp(ch);
                int32_t cur_int = temp_for_display(ch, cur);
                TextField_PrintfCentered(line_current, center_x, "%ld", (long)cur_int);
                break;
            }
        }
    }

    /* Целевая — всегда числом, независимо от неисправности (ВРЕМЕННО, см.
     * докстринг); физически не пересекается с CURRENT/FAULT_MSG/DISABLED_MSG
     * областью (см. SCREEN_TARGET_Y) — стадию гашения выше не ждёт. */
    uint16_t target = Settings_GetTarget(ch);
    if (enabled && !Error_IsChannelBlocked(ch) && Sleep_GetMode(ch) == SLEEP_MODE_PRESLEEP) {
        /* В PRESLEEP реально применяется min(уставка, PresleepTemp), см.
         * Control_PresleepSetpoint() — для визуального контроля показываем именно её.
         * При выходе из PRESLEEP (любая активность) снова возвращается уставка. */
        target = (uint16_t)FIXED_TO_INT(Control_PresleepSetpoint(ch, FIXED_FROM_INT(target)));
    }
    TextField_PrintfCentered(line_target, center_x, "%u", (unsigned)target);

    if (faulted != (s_last_content[ch] == CHANNEL_CONTENT_FAULT)) {
        apply_channel_colors(ch);
    }
    s_last_content[ch] = content;
}

/**
 * @brief Обновить поле таймера сна одного канала в инфозоне (текст, цвет
 *        И иконку циферблата)
 *
 * Цвет зависит не от `mode` напрямую, а от того, КАКОЙ порог сейчас
 * отсчитывается (таймеры независимы, см. sleep.c/Sleep.md) — один и тот же
 * mode может значить разный цвет:
 *
 * AWAKE, remaining==0 (не простаивает)               -> "" (пусто), без иконки, цвет не виден
 * AWAKE, remaining>0, PreSleepTimeout включён          -> "MM:SS" + иконка, жёлтый  — отсчёт ДО первого (PreSleep)
 * AWAKE, remaining>0, PreSleepTimeout ВЫКЛЮЧЕН         -> "MM:SS" + иконка, красный — первый выключен, это уже "второй" (Sleep) таймер, стартует сразу по простою вместо первого
 * PRESLEEP, remaining==0 (SleepTimeout выключен)       -> "Предсон" + иконка (бессрочно, второго таймера нет), жёлтый
 * PRESLEEP, remaining>0 (первый уже сработал)          -> "MM:SS" + иконка, красный — отсчёт "второго" (Sleep) таймера, а не статичная "Предсон"
 * SLEEP                                                 -> "" (пусто), без иконки — растровая иконка
 *                                                          спящего смайлика на месте температуры уже
 *                                                          достаточный сигнал (см. CHANNEL_CONTENT_ASLEEP в
 *                                                          update_channel_content()), отдельная
 *                                                          подпись "Спит" в инфозоне убрана (см. чат);
 *                                                          физического снижения нагрева при входе
 *                                                          в SLEEP пока нет — см. ниже
 *
 * Текст всегда выравнивается по правому краю right_edge_x. Иконка
 * циферблата ставится СЛЕВА от него вплотную (зазор SLEEP_ICON_GAP_X),
 * поэтому у любого по длине текста ("Предсон"/"M:SS"/"MM:SS") иконка
 * гарантированно не перекрывается текстом.
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
    /* Таймер сна не показывается, когда канал выключен ИЛИ инструмент не
     * подключен (Error_IsChannelIdle(), см. error.h) — у отсутствующего
     * инструмента нет ни таймера, ни статуса сна (ни "Предсон"/"Спит", ни
     * иконки). Модуль Sleep при этом продолжает считать простой сам по себе. */
    bool enabled = State_IsEnabled(ch) && !Error_IsChannelIdle(ch);
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
            /* Раньше был текст "Спит" + иконка рядом (нагрев уже отключён
             * Control'ом) — убрали по запросу (см. чат): прочерков на месте
             * температуры (CHANNEL_CONTENT_ASLEEP, см. update_channel_content())
             * достаточно, отдельная подпись в инфозоне не нужна. Тот же
             * "нечего показывать", что и у AWAKE/remaining==0. */
            buf[0] = '\0';
            timer_visible = false;
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

    display_color_t color = COLOR_SLEEP_AWAKE;
    switch (mode) {
        case SLEEP_MODE_AWAKE:
            /* remaining==0 -> не простаивает, ничего не считаем, цвет не важен
             * (buf пуст). remaining>0 -> если PreSleepTimeout включён, это
             * отсчёт ДО НЕГО (жёлтый, "первый" таймер); если выключен,
             * Sleep_GetRemainingSeconds() в AWAKE уже считает от простоя
             * напрямую до SLEEP (см. sleep.c) — фактически "второй" таймер,
             * красный, стартует сразу по простою вместо первого. */
            if (remaining != 0) {
                color = (Settings_GetPreSleepTimeout(ch) > 0) ? COLOR_SLEEP_PRESLEEP : COLOR_SLEEP_SLEEP;
            }
            break;
        case SLEEP_MODE_PRESLEEP:
            /* remaining==0 -> SleepTimeout выключен, PRESLEEP бессрочно,
             * "второго" таймера нет — жёлтый статичный "Предсон". remaining>0
             * -> первый (PreSleep) уже сработал, это отсчёт "второго"
             * (Sleep) таймера — красный, а не жёлтая "Предсон". */
            color = (remaining == 0) ? COLOR_SLEEP_PRESLEEP : COLOR_SLEEP_SLEEP;
            break;
        case SLEEP_MODE_SLEEP:
            color = COLOR_SLEEP_SLEEP;
            break;
        default:
            break;
    }
    if (color != s_last_sleep_color[ch]) {
        TextField_SetColors(line, color, COLOR_BG);
        s_last_sleep_color[ch] = color;
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
            if (i < 4) {
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
