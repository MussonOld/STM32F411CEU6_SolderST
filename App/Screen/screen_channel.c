/**
 * @file    screen_channel.c
 * @brief   Отрисовка каналов главного экрана — см. screen_channel.h; раскладка и
 *          состояния CURRENT-блока описаны в шапке screen.c.
 */
#include "screen_channel.h"
#include "screen_layout.h"
#include "screen_icons.h"
#include "screen.h"
#include "text_field.h"
#include "display.h"
#include "fonts.h"
#include "state.h"
#include "settings.h"
#include "fsm.h"
#include "error.h"
#include "sleep.h"
#include "control.h"
#include "fixed_point.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static channel_id_t s_last_active_channel;

/**
 * @brief Взаимоисключающие состояния содержимого CURRENT-блока канала
 *        (см. update_channel_content()). Переключение между ЛЮБОЙ парой
 *        состояний требует гашения текстовых полей и ожидания settled (см.
 *        s_content_clearing ниже): каждое состояние рисует CURRENT-блок
 *        по-своему (число, 2-строчный текст аварии, "ВЫКЛ" отдельным полем,
 *        крестик и иконка сна — растрами), и все они физически делят одну
 *        Y-полосу.
 */
typedef enum {
    CHANNEL_CONTENT_NORMAL = 0, /* число текущей температуры (Comic_60_dig) */
    CHANNEL_CONTENT_FAULT,      /* авария — 2-строчное сообщение (AntiquaB_18_uni) */
    CHANNEL_CONTENT_IDLE,       /* инструмент не подключен — растровый крестик, без текста */
    CHANNEL_CONTENT_DISABLED,   /* канал выключен аккордом — "ВЫКЛ" (AntiquaB_32_uni) */
    CHANNEL_CONTENT_ASLEEP,     /* SLEEP_MODE_SLEEP — растровая иконка (спящий смайлик, см. sleep_icon.h) */
    CHANNEL_CONTENT_PRESLEEP,   /* SLEEP_MODE_PRESLEEP — растровая иконка (зевающий смайлик, см. presleep_icon.h) */
} channel_content_t;

static channel_content_t s_last_content[CHANNEL_COUNT]; /* чтобы перекрашивать title/current и гасить блок только при реальной смене состояния */
static bool s_content_clearing[CHANNEL_COUNT]; /* см. update_channel_content() — гасим CURRENT/FAULT_MSG/FAULT_MSG2/DISABLED_MSG
                                                 * (и крестик, отдельно, см. s_idle_icon_shown) перед сменой состояния, чтобы
                                                 * стирание одного поля не затёрло уже нарисованное содержимое другого (они
                                                 * физически перекрываются по Y, см. докстринг файла) */
static bool s_idle_icon_shown[CHANNEL_COUNT]; /* крестик "инструмент не подключен" сейчас реально нарисован на экране (растр, см. ScreenIcons_DrawIdleCross()) —
                                                * рисуется/стирается сырым Display_WritePixelsDMA в обход TextField, поэтому
                                                * TextField не знает про эти пиксели и не сотрёт их сам при смене состояния (см. update_channel_content()) */
static bool s_asleep_icon_shown[CHANNEL_COUNT]; /* иконка SLEEP (спящий смайлик) сейчас реально нарисована — тот же смысл
                                                  * и та же причина, что и у s_idle_icon_shown (см. ScreenIcons_DrawAsleep()) */
static bool s_presleep_icon_shown[CHANNEL_COUNT]; /* иконка PRESLEEP (зевающий смайлик) сейчас реально нарисована — тот же смысл (см. ScreenIcons_DrawPresleep()) */
static display_color_t s_last_sleep_color[CHANNEL_COUNT]; /* чтобы перекрашивать таймер сна только при реальной смене цвета (не режима — один и тот же mode может значить разный цвет, см. update_sleep_status()) */
static uint16_t s_sleep_icon_x[CHANNEL_COUNT]; /* x, по которому иконка РЕАЛЬНО сейчас нарисована на экране (актуален только пока s_sleep_icon_shown[ch]==true) — пересчитывается в update_sleep_status() */
static bool s_sleep_icon_shown[CHANNEL_COUNT]; /* сейчас ли иконка реально нарисована на экране (скрыта, когда таймер не отображается) */
static int32_t s_temp_shown[CHANNEL_COUNT];      /* целое, которое сейчас показывает CURRENT (актуально только при s_temp_shown_valid) */
static bool    s_temp_shown_valid[CHANNEL_COUNT]; /* false, пока CURRENT показывает не число (авария/крестик/"ВЫКЛ"/иконка сна) — следующее число берётся без гистерезиса */

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
 *        всегда числом и обычной активной/неактивной окраской (см. шапку
 *        файла). Пресеты тоже не входят — общие поля.
 */
static void apply_channel_colors(channel_id_t ch)
{
    uint8_t line_title, line_current, line_disabled_msg, line_sleep_temp;
    bool active = (ch == InputFSM_GetActiveChannel());
    bool faulted = Error_IsChannelFaulted(ch);

    if (ch == CHANNEL_SOLDER) {
        line_title        = LINE_SOLDER_TITLE;
        line_current      = LINE_SOLDER_CURRENT;
        line_disabled_msg = LINE_SOLDER_DISABLED_MSG;
        line_sleep_temp   = LINE_SOLDER_SLEEP_TEMP;
    } else {
        line_title        = LINE_DESOLDER_TITLE;
        line_current      = LINE_DESOLDER_CURRENT;
        line_disabled_msg = LINE_DESOLDER_DISABLED_MSG;
        line_sleep_temp   = LINE_DESOLDER_SLEEP_TEMP;
    }

    if (faulted) {
        TextField_SetColors(line_title,   COLOR_FAULT, COLOR_BG); /* заголовок — иконка (см. ScreenIcons_DrawSolderTitle()/ScreenIcons_DrawDesolderTitle()), эта строка ничего не красит на экране — no-op, оставлено чтобы не усложнять функцию веткой на канал */
        TextField_SetColors(line_current, COLOR_FAULT, COLOR_BG);
        TextField_SetColors(line_sleep_temp, COLOR_FAULT, COLOR_BG); /* при аварии не показывается (фазы сна ниже приоритетом), цвет — для порядка */
    } else {
        TextField_SetColors(line_title,   active ? COLOR_ACTIVE_TITLE   : COLOR_INACTIVE_TITLE,   COLOR_BG); /* тот же no-op, что и выше */
        TextField_SetColors(line_current, active ? COLOR_ACTIVE_CURRENT : COLOR_INACTIVE_CURRENT, COLOR_BG);
        /* CHANNEL_CONTENT_DISABLED ("ВЫКЛ") — та же активная/неактивная
         * пара, что и у CURRENT (авария и disabled взаимоисключающие, см.
         * update_channel_content(), так что faulted-ветку сюда заводить не нужно). */
        TextField_SetColors(line_disabled_msg, active ? COLOR_ACTIVE_CURRENT : COLOR_INACTIVE_CURRENT, COLOR_BG);
        TextField_SetColors(line_sleep_temp, active ? COLOR_ACTIVE_SLEEP_TEMP : COLOR_INACTIVE_SLEEP_TEMP, COLOR_BG); /* температура в фазах сна — серая пара (см. COLOR_ACTIVE_SLEEP_TEMP) */
    }
}

static void apply_target_colors(channel_id_t ch)
{
    uint8_t line_target = (ch == CHANNEL_SOLDER) ? LINE_SOLDER_TARGET : LINE_DESOLDER_TARGET;
    bool active = (ch == InputFSM_GetActiveChannel());
    TextField_SetColors(line_target, active ? COLOR_ACTIVE_TARGET : COLOR_INACTIVE_TARGET, COLOR_BG);
}

/* Последнее нарисованное заполнение гейджа мощности, в пикселях — чтобы не
 * дёргать DMA заново на каждый Screen_Update(), если сглаженный процент
 * (см. Control_GetSmoothedPowerPct()) после квантования в пиксели не
 * изменился. 0xFFFF — недостижимое значение (высота гейджа < 0xFFFF),
 * форсирует перерисовку целиком на первый вызов после Screen_Init()/
 * возврата из меню (см. clear_screen_for_mode_switch()). */
static uint16_t s_power_bar_fill_px[CHANNEL_COUNT];

/** @brief Один сплошной сегмент гейджа (ширина фиксирована SCREEN_POWER_BAR_WIDTH), height==0 — no-op. */
static bool draw_power_bar_segment(uint16_t x0, uint16_t y0, uint16_t height, display_color_t color)
{
    if (height == 0U) return true;
    if (Display_SetWindow(x0, y0, (uint16_t)(x0 + SCREEN_POWER_BAR_WIDTH - 1U),
                           (uint16_t)(y0 + height - 1U)) != DISPLAY_OK) {
        return false;
    }
    if (Display_FillColorDMA(color, (uint32_t)SCREEN_POWER_BAR_WIDTH * height) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { } /* редкое событие — только когда сглаженный % реально сдвинул пиксель заполнения, см. вызов ниже */
    return true;
}

/**
 * @brief Перерисовать гейдж мощности канала — сегмент "трек" (тусклый,
 *        сверху) + сегмент "заполнение" (снизу, см. геометрию выше), но
 *        только если высота заполнения в пикселях изменилась с прошлого
 *        раза (s_power_bar_fill_px). Redraw целиком, а не только дельту —
 *        колонка узкая (SCREEN_POWER_BAR_WIDTH), а меняется редко благодаря
 *        сглаживанию в Control (секунды, см. CONTROL_POWER_DISPLAY_FILTER_MS) —
 *        лишний DMA-трафик от передельки уже закрашенного несущественный.
 */
static void update_power_gauge(channel_id_t ch, uint16_t bar_x0)
{
    fixed_t pct = Control_GetSmoothedPowerPct(ch); /* 0..100, Q16.16 */
    fixed_t fill_fixed = fixed_div(fixed_mul(pct, FIXED_FROM_INT((int32_t)SCREEN_POWER_BAR_HEIGHT)),
                                    FIXED_FROM_INT(100));
    int32_t fill_px_signed = FIXED_TO_INT(fill_fixed + (FIXED_ONE >> 1)); /* округление */
    if (fill_px_signed < 0) fill_px_signed = 0;
    if (fill_px_signed > (int32_t)SCREEN_POWER_BAR_HEIGHT) fill_px_signed = (int32_t)SCREEN_POWER_BAR_HEIGHT;
    uint16_t fill_px = (uint16_t)fill_px_signed;

    if (fill_px == s_power_bar_fill_px[ch]) {
        return; /* пиксель заполнения не изменился — перерисовывать нечего */
    }

    uint16_t track_height = (uint16_t)(SCREEN_POWER_BAR_HEIGHT - fill_px);
    bool ok = draw_power_bar_segment(bar_x0, SCREEN_POWER_BAR_Y0, track_height, COLOR_POWER_TRACK);
    ok = draw_power_bar_segment(bar_x0, (uint16_t)(SCREEN_POWER_BAR_Y0 + track_height), fill_px, COLOR_POWER_FILL) && ok;
    if (ok) {
        s_power_bar_fill_px[ch] = fill_px; /* при ошибке не фиксируем — следующий вызов перерисует */
    }
}

/**
 * @brief Аварийное сообщение всегда в 2 строки — 2 слова в одну строку не
 *        помещаются. Делит msg по первому пробелу: "Обрыв
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

/* Линии TextField одного канала. Раньше выбирались шестью тернарниками в самой
 * update_channel_content() — теперь одна таблица, функциям ниже достаточно
 * указателя на неё. */
typedef struct {
    uint8_t current;      /* число (Comic_60_dig) */
    uint8_t fault_msg;    /* 1-я строка сообщения аварии */
    uint8_t fault_msg2;   /* 2-я строка сообщения аварии */
    uint8_t disabled_msg; /* "ВЫКЛ" */
    uint8_t target;       /* уставка (временное отладочное поле) */
    uint8_t sleep_temp;   /* текущая температура в PRESLEEP/SLEEP */
} channel_lines_t;

static const channel_lines_t s_channel_lines[CHANNEL_COUNT] = {
    [CHANNEL_SOLDER] = {
        LINE_SOLDER_CURRENT, LINE_SOLDER_FAULT_MSG, LINE_SOLDER_FAULT_MSG2,
        LINE_SOLDER_DISABLED_MSG, LINE_SOLDER_TARGET, LINE_SOLDER_SLEEP_TEMP,
    },
    [CHANNEL_DESOLDER] = {
        LINE_DESOLDER_CURRENT, LINE_DESOLDER_FAULT_MSG, LINE_DESOLDER_FAULT_MSG2,
        LINE_DESOLDER_DISABLED_MSG, LINE_DESOLDER_TARGET, LINE_DESOLDER_SLEEP_TEMP,
    },
};

/* X левого края растров, центрируемых по оси половины экрана этого канала. */
static uint16_t idle_cross_x(uint16_t center_x)     { return (uint16_t)(center_x - SCREEN_IDLE_CROSS_SIZE / 2U); }
static uint16_t asleep_icon_x(uint16_t center_x)    { return (uint16_t)(center_x - SLEEP_ICON_BITMAP_W / 2U); }
static uint16_t presleep_icon_x(uint16_t center_x)  { return (uint16_t)(center_x - PRESLEEP_ICON_BITMAP_W / 2U); }

typedef bool (*raster_op_fn)(uint16_t x, uint16_t y);

/**
 * @brief Что сейчас показывать на месте CURRENT канала.
 *
 * Приоритет состояний: авария > idle > выключен аккордом >
 * спит (SLEEP_MODE_SLEEP) > предсон (SLEEP_MODE_PRESLEEP) > обычное число.
 * idle/fault физически не пересекаются (Error_GetToolFault() — одно значение
 * на канал, см. error.h), остальные пары — независимые подсистемы
 * (State/Sleep), поэтому порядок проверок важен.
 */
static channel_content_t classify_channel_content(channel_id_t ch, const char *fault_msg)
{
    if (fault_msg != NULL)       return CHANNEL_CONTENT_FAULT;
    if (Error_IsChannelIdle(ch)) return CHANNEL_CONTENT_IDLE; /* инструмент не подключен — не авария, крестик (см. ScreenIcons_DrawIdleCross()) */
    if (!State_IsEnabled(ch))    return CHANNEL_CONTENT_DISABLED;

    sleep_mode_t mode = Sleep_GetMode(ch);
    if (mode == SLEEP_MODE_SLEEP)    return CHANNEL_CONTENT_ASLEEP;
    if (mode == SLEEP_MODE_PRESLEEP) return CHANNEL_CONTENT_PRESLEEP;
    return CHANNEL_CONTENT_NORMAL;
}

/** @brief Показывает ли это состояние число температуры (CURRENT либо SLEEP_TEMP) — от этого зависит гистерезис. */
static bool content_shows_temp(channel_content_t content)
{
    return content == CHANNEL_CONTENT_NORMAL || content == CHANNEL_CONTENT_PRESLEEP
        || content == CHANNEL_CONTENT_ASLEEP;
}

/**
 * @brief Стереть растровую иконку состояния на выходе из него.
 *
 * Крестик IDLE, иконки SLEEP и PRESLEEP рисуются СЫРЫМ Display_WritePixelsDMA в
 * обход TextField (см. s_idle_icon_shown и пр.) — TextField не знает про эти
 * пиксели и не сотрёт их сам ни на выходе из состояния, ни держа старое
 * число под ними, поэтому стирание вызывается явно на выходе, ДО того как
 * фаза гашения текстовых полей (content_swap_settled()) вообще запускается —
 * иначе иконка остаётся видимой поверх нового текста ещё один кадр. Растр
 * PRESLEEP уже иконки SLEEP (58 против 70px), поэтому у каждой иконки свой
 * прямоугольник стирания. Флаг сбрасывается только по факту успешного стирания.
 */
static void leave_raster_state(channel_content_t last, channel_content_t state, bool *shown,
                               raster_op_fn erase, uint16_t x, uint16_t y)
{
    if (last == state && *shown && erase(x, y)) {
        *shown = false;
    }
}

static void erase_icons_on_exit(channel_id_t ch, channel_content_t content, uint16_t center_x)
{
    channel_content_t last = s_last_content[ch];
    if (last == content) {
        return; /* выхода нет — ни одно из условий ниже выполниться не может */
    }
    leave_raster_state(last, CHANNEL_CONTENT_IDLE, &s_idle_icon_shown[ch],
                       ScreenIcons_EraseIdleCross, idle_cross_x(center_x), SCREEN_IDLE_CROSS_Y);
    leave_raster_state(last, CHANNEL_CONTENT_ASLEEP, &s_asleep_icon_shown[ch],
                       ScreenIcons_EraseAsleep, asleep_icon_x(center_x), SCREEN_ASLEEP_ICON_Y);
    leave_raster_state(last, CHANNEL_CONTENT_PRESLEEP, &s_presleep_icon_shown[ch],
                       ScreenIcons_ErasePresleep, presleep_icon_x(center_x), SCREEN_PRESLEEP_ICON_Y);
}

/** @brief Нарисовать растр, если он ещё не на экране; флаг ставится только по факту успеха. */
static void show_raster_once(bool *shown, raster_op_fn draw, uint16_t x, uint16_t y)
{
    if (!*shown && draw(x, y)) {
        *shown = true;
    }
}

/**
 * @brief Фаза гашения на смене состояния. true — можно рисовать новое содержимое.
 *
 * CURRENT (Comic_60_dig), FAULT_MSG/FAULT_MSG2 (AntiquaB_18_uni),
 * DISABLED_MSG (AntiquaB_32_uni) и растровые иконки (см. leave_raster_state())
 * физически делят одну и ту же Y-полосу (см. докстринг файла) — в любой
 * момент непусто ровно одно из них. TextField_Process() отрисовывает по
 * одной dirty-строке за вызов, начиная с САМОГО МЕЛКОГО индекса (см.
 * text_field.c) — у CURRENT индекс меньше, чем у FAULT_MSG/2/DISABLED_MSG.
 * Если писать новое содержимое напрямую на кадре смены состояния, при
 * ПОЯВЛЕНИИ более позднего по индексу поля порядок безопасен (CURRENT
 * первым гасится на "", остальные рисуют текст на уже пустом месте), а
 * вот при возврате К CURRENT порядок ломается: CURRENT (меньший индекс)
 * рисуется первым и получает число, а прежнее поле гасится только
 * ПОСЛЕ — и его стирание старого текста (та же Y-полоса!) затирает уже
 * нарисованное. Статичным порядком индексов это не решить (для
 * противоположного перехода порядок снова стал бы неверным) — поэтому на
 * самом переходе сначала гасим ВСЕ ТЕКСТОВЫЕ поля и ждём, пока реально
 * доиграет отрисовка (TextField_IsSettled()), и только потом на
 * следующих вызовах рисуем настоящее новое содержимое.
 */
static bool content_swap_settled(channel_id_t ch, channel_content_t content,
                                 const channel_lines_t *ln, uint16_t center_x)
{
    if (!s_content_clearing[ch] && content != s_last_content[ch]) {
        TextField_PrintfCentered(ln->current, center_x, "");
        TextField_PrintfCentered(ln->fault_msg, center_x, "");
        TextField_PrintfCentered(ln->fault_msg2, center_x, "");
        TextField_PrintfCentered(ln->disabled_msg, center_x, "");
        s_content_clearing[ch] = true;
    }

    if (s_content_clearing[ch]
        && TextField_IsSettled(ln->current) && TextField_IsSettled(ln->fault_msg)
        && TextField_IsSettled(ln->fault_msg2) && TextField_IsSettled(ln->disabled_msg)) {
        s_content_clearing[ch] = false;
    }

    return !s_content_clearing[ch];
}

/** @brief Текущая температура под иконкой сна/предсна (LINE_x_SLEEP_TEMP), с тем же гистерезисом, что и у CURRENT. */
static void print_sleep_temp(channel_id_t ch, const channel_lines_t *ln, uint16_t center_x)
{
    TextField_PrintfCentered(ln->sleep_temp, center_x, "%ld",
                             (long)temp_for_display(ch, State_GetCurrentTemp(ch)));
}

/** @brief Нарисовать содержимое канала для УЖЕ установившегося состояния (после фазы гашения). */
static void draw_channel_content(channel_id_t ch, channel_content_t content,
                                 const channel_lines_t *ln, uint16_t center_x, const char *fault_msg)
{
    /* В фазах сна число тоже показывается (LINE_x_SLEEP_TEMP), с тем же
     * гистерезисом — состояние гистерезиса при входе/выходе из PRESLEEP/SLEEP
     * сохраняется, чтобы число не «прыгало» на границе фаз. */
    if (!content_shows_temp(content)) {
        s_temp_shown_valid[ch] = false; /* число не показываем — при возврате начинаем без гистерезиса */
    }
    switch (content) {
        case CHANNEL_CONTENT_FAULT:
            /* Текущая температура НЕ выводится вообще — на её месте сообщение
             * в отдельном поле (Comic_60_dig кириллицу не содержит), в 2 строки. */
            print_fault_message_2line(ln->fault_msg, ln->fault_msg2, center_x, fault_msg);
            break;
        case CHANNEL_CONTENT_IDLE:
            /* Инструмент не подключен: ни красного, ни зуммера (см.
             * error.h) — растровый крестик пониженной контрастности
             * (см. COLOR_IDLE_CROSS) вместо текста. */
            show_raster_once(&s_idle_icon_shown[ch], ScreenIcons_DrawIdleCross, idle_cross_x(center_x), SCREEN_IDLE_CROSS_Y);
            break;
        case CHANNEL_CONTENT_DISABLED:
            /* Канал выключен коротким UP+DN (см. fsm.c) — "ВЫКЛ"
             * отдельным полем AntiquaB_32_uni (Comic_60_dig кириллицу не
             * содержит, как и у сообщений аварии). */
            TextField_PrintfCentered(ln->disabled_msg, center_x, "ВЫКЛ");
            break;
        case CHANNEL_CONTENT_ASLEEP:
            /* SLEEP_MODE_SLEEP — растровая иконка (спящий смайлик, см.
             * sleep_icon.h) сырым Display_WritePixelsDMA в обход TextField —
             * тот же паттерн, что и у крестика IDLE. Нагрев в SLEEP
             * отключает Control — иконка только индицирует состояние. */
            show_raster_once(&s_asleep_icon_shown[ch], ScreenIcons_DrawAsleep, asleep_icon_x(center_x), SCREEN_ASLEEP_ICON_Y);
            print_sleep_temp(ch, ln, center_x);
            break;
        case CHANNEL_CONTENT_PRESLEEP:
            /* SLEEP_MODE_PRESLEEP — растровая иконка (зевающий смайлик, см.
             * presleep_icon.h) вместо большого числа, тот же паттерн, что и
             * у SLEEP. Нагрев продолжается (на сниженной уставке, см.
             * Control_PresleepSetpoint()), поэтому текущая температура
             * остаётся видна — под иконкой, как и в SLEEP. */
            show_raster_once(&s_presleep_icon_shown[ch], ScreenIcons_DrawPresleep, presleep_icon_x(center_x), SCREEN_PRESLEEP_ICON_Y);
            print_sleep_temp(ch, ln, center_x);
            break;
        case CHANNEL_CONTENT_NORMAL:
        default:
            TextField_PrintfCentered(ln->current, center_x, "%ld",
                                     (long)temp_for_display(ch, State_GetCurrentTemp(ch)));
            break;
    }
}

/**
 * @brief Уставка для показа: в PRESLEEP реально применяется min(уставка,
 *        PresleepTemp), см. Control_PresleepSetpoint() — для визуального
 *        контроля показываем именно её. Из PRESLEEP канал выходит только по
 *        активности на входах Sleep — инструмент снят с подставки (Dock) или
 *        нажата кнопка помпы (Btn_Pump), а также при включении канала
 *        (Sleep_ForceAwake()); тогда снова показывается уставка.
 */
static uint16_t target_for_display(channel_id_t ch, bool enabled)
{
    uint16_t target = Settings_GetTarget(ch);
    if (enabled && !Error_IsChannelBlocked(ch) && Sleep_GetMode(ch) == SLEEP_MODE_PRESLEEP) {
        target = (uint16_t)FIXED_TO_INT(Control_PresleepSetpoint(ch, FIXED_FROM_INT(target)));
    }
    return target;
}

/**
 * @brief Обновить содержимое канала: title-цвет, current/fault_msg (ровно
 *        одно из двух непусто), target (всегда число)
 * @param center_x Центр половины экрана этого канала
 */
static void update_channel_content(channel_id_t ch, uint16_t center_x)
{
    const channel_lines_t *ln = &s_channel_lines[ch];
    const char *fault_msg = Error_GetChannelFaultMessage(ch); /* не NULL только для аварий: RTD_SHORT/RTD_OPEN/HEATER_OPEN/ERR ADS1220 */
    bool enabled = State_IsEnabled(ch);
    bool faulted = Error_IsChannelFaulted(ch); /* нужно для перекраски title/current в конце функции, см. apply_channel_colors() */
    channel_content_t content = classify_channel_content(ch, fault_msg);

    erase_icons_on_exit(ch, content, center_x);

    if (content_swap_settled(ch, content, ln, center_x)) {
        draw_channel_content(ch, content, ln, center_x, fault_msg);
    }

    /* Температура в фазах сна гасится сразу, как только канал вышел из
     * PRESLEEP/SLEEP (в том числе на кадрах фазы гашения выше) — поле не
     * делит область с CURRENT/FAULT_MSG/DISABLED_MSG/иконками, порядок
     * отрисовки с ними неважен. */
    if (content != CHANNEL_CONTENT_PRESLEEP && content != CHANNEL_CONTENT_ASLEEP) {
        TextField_PrintfCentered(ln->sleep_temp, center_x, "");
    }

    /* Целевая — всегда числом, независимо от неисправности (см. шапку
     * файла); физически не пересекается с CURRENT/FAULT_MSG/DISABLED_MSG
     * областью (см. SCREEN_TARGET_Y) — стадию гашения выше не ждёт. */
    TextField_PrintfCentered(ln->target, center_x, "%u", (unsigned)target_for_display(ch, enabled));

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
 * SLEEP                                                 -> "" (пусто), без иконки — состояние SLEEP
 *                                                          показывает иконка на месте температуры (см.
 *                                                          CHANNEL_CONTENT_ASLEEP в update_channel_content())
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
 * (новый buf, посчитанный чуть выше, туда ещё не долетел). См. докстринг
 * TextField_GetShownWidth()/TextField_IsSettled() в text_field.h.
 */
/** @brief Что показывать в инфозоне для таймера сна: текст, нужна ли иконка циферблата, цвет. */
typedef struct {
    char            text[16];
    bool            timer_visible;
    display_color_t color;
} sleep_status_view_t;

static void format_sleep_timer(char *buf, size_t size, uint32_t remaining)
{
    snprintf(buf, size, "%lu:%02lu", (unsigned long)(remaining / 60U), (unsigned long)(remaining % 60U));
}

/**
 * @brief Посчитать вид таймера сна канала (см. таблицу состояний в докстринге
 *        update_sleep_status()). Цвет зависит не от mode напрямую, а от того,
 *        КАКОЙ порог сейчас отсчитывается.
 */
static void sleep_status_view(channel_id_t ch, sleep_status_view_t *v)
{
    /* Таймер сна не показывается, когда канал выключен ИЛИ заблокирован
     * (Error_IsChannelBlocked(), см. error.h: инструмент не подключен,
     * неисправен либо авария БП) — у такого канала нет ни таймера, ни
     * статуса сна (ни "Предсон"/"Спит", ни иконки). Sleep сам держит
     * заблокированный канал в AWAKE без таймера, так что проверка здесь —
     * страховка на долю такта до его реакции. Выключенный
     * аккордом UP+DN канал "уснуть" не может (физически не греет и так):
     * ведём себя как AWAKE без простоя — пусто, без иконки; Sleep_GetMode()/
     * Sleep_GetRemainingSeconds() для него не зовём вовсе. */
    bool active = State_IsEnabled(ch) && !Error_IsChannelBlocked(ch);
    sleep_mode_t mode = active ? Sleep_GetMode(ch) : SLEEP_MODE_AWAKE;
    uint32_t remaining = active ? Sleep_GetRemainingSeconds(ch) : 0;

    v->text[0] = '\0';
    v->timer_visible = true;
    v->color = COLOR_SLEEP_AWAKE;

    switch (mode) {
        case SLEEP_MODE_AWAKE:
            if (remaining == 0) {
                v->timer_visible = false; /* нечего показывать — таймер не идёт; цвет не важен */
            } else {
                format_sleep_timer(v->text, sizeof(v->text), remaining);
                /* Если PreSleepTimeout включён, это отсчёт ДО НЕГО (жёлтый,
                 * "первый" таймер); если выключен, Sleep_GetRemainingSeconds() в
                 * AWAKE уже считает от простоя напрямую до SLEEP (см. sleep.c) —
                 * фактически "второй" таймер, красный, стартует сразу по простою
                 * вместо первого. */
                v->color = (Settings_GetPreSleepTimeout(ch) > 0) ? COLOR_SLEEP_PRESLEEP : COLOR_SLEEP_SLEEP;
            }
            break;
        case SLEEP_MODE_PRESLEEP:
            /* remaining==0 -> SleepTimeout выключен, PRESLEEP бессрочно, "второго"
             * таймера нет — жёлтый статичный "Предсон". remaining>0 -> первый
             * (PreSleep) уже сработал, это отсчёт "второго" (Sleep) таймера —
             * красный, а не жёлтая "Предсон". */
            if (remaining == 0) {
                snprintf(v->text, sizeof(v->text), "Предсон");
                v->color = COLOR_SLEEP_PRESLEEP;
            } else {
                format_sleep_timer(v->text, sizeof(v->text), remaining);
                v->color = COLOR_SLEEP_SLEEP;
            }
            break;
        case SLEEP_MODE_SLEEP:
            /* SLEEP показывает иконка на месте температуры (CHANNEL_CONTENT_ASLEEP,
             * см. update_channel_content()), в инфозоне показывать нечего. */
            v->timer_visible = false;
            v->color = COLOR_SLEEP_SLEEP;
            break;
        default:
            break;
    }
}

/** @brief Стереть иконку циферблата со старого места и, если нужна, нарисовать на новом. Флаги — по факту успеха каждой операции. */
static void redraw_sleep_icon(channel_id_t ch, bool timer_visible, uint16_t new_icon_x)
{
    if (s_sleep_icon_shown[ch] && ScreenIcons_EraseClock(s_sleep_icon_x[ch], SLEEP_ICON_Y)) {
        s_sleep_icon_shown[ch] = false; /* точно стёрта, старое место чистое */
    }
    if (timer_visible && !s_sleep_icon_shown[ch] && ScreenIcons_DrawClock(new_icon_x, SLEEP_ICON_Y)) {
        s_sleep_icon_x[ch] = new_icon_x;
        s_sleep_icon_shown[ch] = true;
    }
}

/**
 * @brief Привести иконку циферблата к желаемому виду (нужна/не нужна, позиция).
 *
 * Трогаем иконку только когда строка settled — см. докстринг
 * update_sleep_status() и TextField_IsSettled() в text_field.h: пока не settled
 * (текст только что запрошен, но предыдущее задание для этой строки ещё не
 * доиграно), иконка остаётся там, где её оставило предыдущее settled-состояние,
 * и это безопасно по определению.
 *
 * Состояние (s_sleep_icon_shown[]/s_sleep_icon_x[]) фиксируем ТОЛЬКО по факту
 * успеха каждой операции — не "оптимистично". Если стирание/рисование не
 * удалось, оставляем состояние как было (или как получилось после частичного
 * успеха), чтобы следующий settled-вызов сам повторил недостающий шаг.
 */
static void sync_sleep_icon(channel_id_t ch, uint8_t line, uint16_t right_edge_x, bool timer_visible)
{
    if (!TextField_IsSettled(line)) {
        return;
    }

    uint16_t new_icon_x = s_sleep_icon_x[ch];
    if (timer_visible) {
        /* Строка settled -> shown_text уже равен buf -> ширина финальная,
         * реально нарисованная. Никакого max() с шириной buf не нужно —
         * это одна и та же ширина. */
        uint16_t shown_w = TextField_GetShownWidth(line);
        new_icon_x = (uint16_t)(right_edge_x - shown_w - SLEEP_ICON_GAP_X - SLEEP_ICON_W);
    }

    /* Нужна правка: видимость не совпала, либо иконка видна и должна сдвинуться
     * (при timer_visible == s_sleep_icon_shown[] "иконка видна" == timer_visible). */
    bool stale = (timer_visible != s_sleep_icon_shown[ch])
              || (timer_visible && new_icon_x != s_sleep_icon_x[ch]);
    if (stale) {
        redraw_sleep_icon(ch, timer_visible, new_icon_x);
    }
}

static void update_sleep_status(channel_id_t ch, uint8_t line, uint16_t right_edge_x)
{
    sleep_status_view_t view;
    sleep_status_view(ch, &view);

    TextField_PrintfRightAligned(line, right_edge_x, "%s", view.text);
    sync_sleep_icon(ch, line, right_edge_x, view.timer_visible);

    if (view.color != s_last_sleep_color[ch]) {
        TextField_SetColors(line, view.color, COLOR_BG);
        s_last_sleep_color[ch] = view.color;
    }
}

void ScreenChannel_Init(void)
{
    assert(Comic_60_dig.height == SCREEN_CURRENT_FONT_HEIGHT); /* см. SCREEN_CURRENT_TEXT_Y */

    /* Гейдж мощности — сентинел форсирует первую отрисовку в первом же
     * Screen_Update() (см. update_power_gauge()/докстринг s_power_bar_fill_px). */
    s_power_bar_fill_px[CHANNEL_SOLDER] = 0xFFFFU;
    s_power_bar_fill_px[CHANNEL_DESOLDER] = 0xFFFFU;

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

    /* LINE_SOLDER_TITLE текст не печатает (заголовок — иконка, см.
     * ScreenIcons_DrawSolderTitle()/apply_channel_colors()) — TextField-строка
     * сконфигурирована только чтобы TextField_SetColors() в
     * apply_channel_colors() (безвредный no-op на пустой строке) не трогал
     * неинициализированную линию; x/y и шрифт значения не имеют. */
    TextField_ConfigureLine(LINE_SOLDER_TITLE, SCREEN_HALF_CENTER_LEFT_X, SCREEN_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_ACTIVE_TITLE, COLOR_BG);
    /* CURRENT — Comic_60_dig, крупные цифры. Кириллицу
     * этот шрифт не содержит физически, поэтому для текста об обрыве
     * заведено ОТДЕЛЬНОЕ поле FAULT_MSG (AntiquaB_18_uni), на той же
     * позиции — в любой момент содержимое имеет ровно одно из двух полей,
     * второе пустое (см. update_channel_content()). */
    TextField_ConfigureLine(LINE_SOLDER_CURRENT, SCREEN_HALF_CENTER_LEFT_X, SCREEN_CURRENT_TEXT_Y,
                             &Comic_60_dig, COLOR_ACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_FAULT_MSG, SCREEN_HALF_CENTER_LEFT_X, SCREEN_CURRENT_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_FAULT_MSG2, SCREEN_HALF_CENTER_LEFT_X, SCREEN_FAULT_MSG2_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_DISABLED_MSG, SCREEN_HALF_CENTER_LEFT_X, SCREEN_DISABLED_MSG_Y,
                             &AntiquaB_32_uni, COLOR_ACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_TARGET, SCREEN_HALF_CENTER_LEFT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_ACTIVE_TARGET, COLOR_BG);
    TextField_ConfigureLine(LINE_SOLDER_SLEEP_TEMP, SCREEN_HALF_CENTER_LEFT_X, SCREEN_SLEEP_TEMP_Y,
                             &AntiquaB_32_uni, COLOR_ACTIVE_SLEEP_TEMP, COLOR_BG);

    /* LINE_DESOLDER_TITLE — тот же no-op, что и у LINE_SOLDER_TITLE выше. */
    TextField_ConfigureLine(LINE_DESOLDER_TITLE, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_TITLE_Y,
                             &AntiquaB_18_uni, COLOR_INACTIVE_TITLE, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_CURRENT, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_CURRENT_TEXT_Y,
                             &Comic_60_dig, COLOR_INACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_FAULT_MSG, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_CURRENT_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_FAULT_MSG2, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_FAULT_MSG2_Y,
                             &AntiquaB_18_uni, COLOR_FAULT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_DISABLED_MSG, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_DISABLED_MSG_Y,
                             &AntiquaB_32_uni, COLOR_INACTIVE_CURRENT, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_TARGET, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_TARGET_Y,
                             &AntiquaB_18_uni, COLOR_INACTIVE_TARGET, COLOR_BG);
    TextField_ConfigureLine(LINE_DESOLDER_SLEEP_TEMP, SCREEN_HALF_CENTER_RIGHT_X, SCREEN_SLEEP_TEMP_Y,
                             &AntiquaB_32_uni, COLOR_INACTIVE_SLEEP_TEMP, COLOR_BG);

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_last_content[ch] = CHANNEL_CONTENT_NORMAL; /* Error_Init()/State_Init() тоже гарантируют "нет неисправности"/enabled по умолчанию -> NORMAL */
        s_idle_icon_shown[ch] = false;
        s_last_sleep_color[ch] = COLOR_SLEEP_AWAKE; /* Sleep_Init() тоже гарантирует AWAKE/remaining=0 по умолчанию -> пусто, цвет не важен */
    }
    /* Solder активен по умолчанию при старте (см. InputFSM_Init()) —
     * начальные цвета выше уже расставлены соответственно. */
    s_last_active_channel = CHANNEL_SOLDER;
}

void ScreenChannel_ResetDrawnState(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_sleep_icon_shown[ch] = false;
        s_idle_icon_shown[ch] = false;
        s_asleep_icon_shown[ch] = false;
        s_presleep_icon_shown[ch] = false;
        s_power_bar_fill_px[ch] = 0xFFFFU; /* форс полной перерисовки гейджа, см. его докстринг */
    }
}

void ScreenChannel_UpdateContent(void)
{
    update_channel_content(CHANNEL_SOLDER, SCREEN_HALF_CENTER_LEFT_X);
    update_channel_content(CHANNEL_DESOLDER, SCREEN_HALF_CENTER_RIGHT_X);

    update_power_gauge(CHANNEL_SOLDER, SCREEN_SOLDER_POWER_BAR_X0);
    update_power_gauge(CHANNEL_DESOLDER, SCREEN_DESOLDER_POWER_BAR_X0);
}

void ScreenChannel_UpdateActiveColors(channel_id_t active)
{
    if (active != s_last_active_channel) {
        apply_channel_colors(s_last_active_channel);
        apply_target_colors(s_last_active_channel);
        apply_channel_colors(active);
        apply_target_colors(active);
        s_last_active_channel = active;
    }
}

void ScreenChannel_UpdateSleepStatus(void)
{
    update_sleep_status(CHANNEL_SOLDER, LINE_INFO_SLEEP_SOLDER, SCREEN_INFO_SLEEP_SOLDER_TEXT_RIGHT_EDGE_X);
    update_sleep_status(CHANNEL_DESOLDER, LINE_INFO_SLEEP_DESOLDER, SCREEN_INFO_SLEEP_DESOLDER_TEXT_RIGHT_EDGE_X);
}
