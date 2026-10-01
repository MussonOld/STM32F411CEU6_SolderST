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
 *                иконка спящего смайлика на месте температуры, см.
 *                CHANNEL_CONTENT_ASLEEP),
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
 *      - в PRESLEEP/SLEEP на месте большого числа — иконка (зевающий/
 *        спящий смайлик, растр), а сама текущая температура — под ней
 *        (LINE_x_SLEEP_TEMP, AntiquaB_32_uni, SCREEN_SLEEP_TEMP_Y)
 *      - целевая температура (шрифт AntiquaB_18_uni) — числом независимо
 *        от неисправности, в строке пресетов между ними (SCREEN_TARGET_Y =
 *        SCREEN_SLEEP_TEMP_Y + SCREEN_TARGET_SHIFT_Y). ВРЕМЕННО: отладочное
 *        поле на период опытной эксплуатации, будет убрано
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
 *    Отсос" — Menu_GetTitle()) + до 8 строк списком друг под другом,
 *    шрифт AntiquaB_18_uni везде. Выбранный пункт подсвечивается цветом
 *    (жёлтый — выбран, красный — редактируется), не текстовым курсором.
 *    Те же 8 строк переиспользуются для строк предупреждения Expert
 *    (Menu_IsShowingExpertWarning()), промта подтверждения сброса
 *    (Menu_IsShowingResetConfirm()) и сообщения о выполненном сбросе
 *    (Menu_IsShowingResetDone()) — отдельных полей под это не заведено.
 *    При КАЖДОЙ смене режима экрана (главный <-> меню, в обе стороны) —
 *    полная заливка фона + TextField_InvalidateAll() (см.
 *    clear_screen_for_mode_switch()), чтобы не было наложения одного
 *    экрана на остатки другого; статический разделитель — часть только
 *    главного экрана, перерисовывается заново при возврате в него.
 *
 *  - Заставка в Standby (Settings_GetSplashMode()==SPLASH_MODE_START_STANDBY,
 *    см. standby_splash_wanted()) — полноэкранная картинка (та же, что при
 *    включении, SplashScreen_Bitmap), целиком перекрывает оба канала. Условие
 *    ("Standby"): нет ни одного подключённого и включённого канала, который не
 *    спит, нет аварии/сбоя БП, и хотя бы один подключённый включённый канал в
 *    SLEEP, и нет несохранённых изменений настроек (Settings_HasPendingChanges()).
 *    Пропадает сама — как только условие перестало выполняться (смена состояния
 *    либо изменение уставки/пресета, пока оно не записано в EEPROM); в сервисном меню не показывается
 *    никогда. Появление/исчезновение — по образцу смены режима экрана: полная
 *    перерисовка через clear_screen_for_mode_switch(SCREEN_MODE_MAIN).
 *
 * Координаты — приближённый вариант по вертикали (не откалиброван визуально
 * на реальном дисплее из этой сессии), по горизонтали — динамическое
 * центрирование/выравнивание по факту измеренной ширины (см. gfx.c/text_field.c).
 */

#include "screen.h"
#include "screen_layout.h"
#include "screen_channel.h"
#include "screen_icons.h"
#include "screen_menu.h"
#include "text_field.h"
#include "display.h"
#include "fonts.h"
#include "channel.h"
#include "state.h"
#include "settings.h"
#include "fsm.h"
#include "error.h"
#include "sleep.h"
#include "splash.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

static screen_mode_t s_last_screen_mode; /* чтобы очищать экран только при реальной смене режима, не каждый кадр */

/**
 * @brief Нарисовать статический вертикальный разделитель (однократно, блокирующе)
 */
static void ScreenDivider_Draw(void)
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
        ScreenDivider_Draw(); /* стёрли вместе со всем экраном — у главного экрана он статический, рисуем заново */
        /* Иконки циферблата и крестика НЕ рисуем принудительно здесь — их
         * видимость зависит от текущего состояния (см.
         * update_sleep_status()/update_channel_content()). Экран уже
         * очищен фоном, поэтому просто сбрасываем "показана" в false: сами
         * растры рисуются сырым DMA в обход TextField, и TextField_InvalidateAll()
         * ниже про них не знает — не сбросив это здесь, ближайший
         * update_sleep_status()/update_channel_content() решил бы, что
         * иконка/крестик уже на экране (раз "показана"=true с прошлого
         * раза), и не перерисовал бы их на самом деле чистом фоне. */
        ScreenChannel_ResetDrawnState();
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
         * экрана (сервисное меню туда и обратно). */
        ScreenIcons_DrawSolderTitle();
        ScreenIcons_DrawDesolderTitle();
    }
}

void Screen_Init(void)
{
    ScreenDivider_Draw();

    TextField_ConfigureLine(LINE_INFO, SCREEN_INFO_X, SCREEN_INFO_Y,
                             &AntiquaB_18_uni, COLOR_INFO, COLOR_BG);

    /* Таймеры сна в инфозоне и оба канала: строки TextField и начальное состояние. */
    ScreenChannel_Init();

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

    /* Заголовки каналов — иконки (растр, раз и навсегда, см.
     * ScreenIcons_DrawSolderTitle()/ScreenIcons_DrawDesolderTitle()), не текст. */
    ScreenIcons_DrawSolderTitle();
    ScreenIcons_DrawDesolderTitle();

    ScreenMenu_Init();

    s_last_screen_mode = SCREEN_MODE_MAIN; /* совпадает с InputFSM_Init() — при первом Screen_Update() лишней очистки не будет */
}

/* ---- Заставка в Standby ---- */

/** сейчас ли заставка Standby реально нарисована поверх главного экрана */
static bool s_standby_splash_shown;

/**
 * @brief Нужна ли сейчас заставка Standby (только главный экран — вызывающий
 *        Screen_Update() в сервисном меню сюда не заходит).
 *
 * Канал не учитывается, если инструмент не подключён (Error_IsChannelIdle())
 * или выключен пользователем (!State_IsEnabled(), нагрева и так нет — на
 * экране "ВЫКЛ"). Остальные ("действующие") каналы должны ВСЕ быть в SLEEP,
 * и таких должен быть хотя бы один — иначе это не Standby. Авария на
 * подключённом канале (или сбой БП — Error_IsChannelBlocked() у подключённого
 * канала) заставку запрещает: сообщение об ошибке не должно быть скрыто.
 */
static bool standby_splash_wanted(void)
{
    if (Settings_GetSplashMode() != SPLASH_MODE_START_STANDBY) {
        return false;
    }
    if (Settings_HasPendingChanges()) {
        /* Есть несохранённые изменения — пользователь только что менял уставку/
         * пресет. Заставка исчезает сразу (на экране видно результат) и
         * возвращается после записи в EEPROM (SETTINGS_SAVE_DELAY_MS после
         * последнего изменения). Канал при этом не просыпается. */
        return false;
    }

    bool any_sleeping = false;
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        channel_id_t ch = (channel_id_t)i;
        if (Error_IsChannelIdle(ch)) {
            continue; /* инструмент не подключён */
        }
        if (Error_IsChannelBlocked(ch)) {
            return false; /* авария/сбой БП — экран должен показывать ошибку */
        }
        if (!State_IsEnabled(ch)) {
            continue; /* выключен аккордом UP+DN */
        }
        if (Sleep_GetMode(ch) != SLEEP_MODE_SLEEP) {
            return false; /* хотя бы один действующий канал не спит */
        }
        any_sleeping = true;
    }
    return any_sleeping;
}

/** @brief Нарисовать заставку на весь экран (блокирует на время DMA-передачи, как clear_screen_for_mode_switch() — редкое событие). true — нарисовано. */
static bool draw_standby_splash(void)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(0, 0, SPLASH_BITMAP_W - 1, SPLASH_BITMAP_H - 1) != DISPLAY_OK) {
        return false;
    }
    if (Display_WritePixelsDMA(SplashScreen_Bitmap, (uint32_t)SPLASH_BITMAP_W * SPLASH_BITMAP_H) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { }
    return true;
}

void Screen_Update(void)
{
    screen_mode_t mode = InputFSM_GetScreenMode();
    if (mode != s_last_screen_mode) {
        s_standby_splash_shown = false; /* смена режима экрана стирает и заливает всё заново — заставка (если была) уже не на экране */
        clear_screen_for_mode_switch(mode);
        s_last_screen_mode = mode;
    }

    if (mode == SCREEN_MODE_SERVICE) {
        ScreenMenu_Render();
        return; /* меню заменяет собой весь главный экран — остальное не обновляем */
    }

    if (standby_splash_wanted()) {
        if (!s_standby_splash_shown) {
            TextField_InvalidateAll(); /* отменить недорисованное задание и обнулить строки — иначе TextField_Process() дорисует текст поверх заставки */
            s_standby_splash_shown = draw_standby_splash();
        }
        if (s_standby_splash_shown) {
            return; /* заставка держит весь экран — остальное не обновляем и не рисуем */
        }
        /* не удалось нарисовать (DMA/окно) — повторим на следующем Screen_Update(), а пока обновляем главный экран как обычно */
    } else if (s_standby_splash_shown) {
        s_standby_splash_shown = false;
        clear_screen_for_mode_switch(SCREEN_MODE_MAIN); /* заставка стёрта заливкой фона, главный экран рисуется заново с нуля */
    }

    ScreenChannel_UpdateContent();

    channel_id_t active = InputFSM_GetActiveChannel();

    /* Пресеты — три отдельных поля, всегда показывают пресеты АКТИВНОГО канала */
    TextField_Printf(LINE_PRESET_1, "%u", (unsigned)Settings_GetPreset(active, PRESET_1));
    TextField_PrintfCentered(LINE_PRESET_2, SCREEN_PRESET2_CENTER_X, "%u", (unsigned)Settings_GetPreset(active, PRESET_2));
    TextField_PrintfRightAligned(LINE_PRESET_3, SCREEN_PRESET3_RIGHT_EDGE_X, "%u", (unsigned)Settings_GetPreset(active, PRESET_3));

    ScreenChannel_UpdateActiveColors(active);

    const char *info_msg = Error_GetInfoZoneMessage();
    TextField_Printf(LINE_INFO, "%s", (info_msg != NULL) ? info_msg : "");

    ScreenChannel_UpdateSleepStatus();
}
