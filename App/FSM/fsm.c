/**
 * @file fsm.c
 * @brief Реализация fsm.h — см. правила в шапке заголовка.
 */

#include <stdint.h>
#include <stdbool.h>
#include "fsm.h"
#include "buttons.h"
#include "settings.h"
#include "state.h"
#include "error.h"
#include "menu.h"
#include "screen.h" /* Screen_GetShownTemp() - отображаемая температура для записи в пресет */
#include "sleep.h"
#include "fixed_point.h"
#include "step_accel.h" /* авто-повтор UP/DN — общий с menu.c, см. App/Common */
#include "stm32f4xx_hal.h" /* HAL_GetTick() — интервал авто-повтора UP/DN */

static channel_id_t s_active_channel;
static screen_mode_t s_screen_mode;

static step_accel_t s_accel;

/* ---- Внутренние примитивы ---- */

static preset_id_t preset_for_button(button_id_t btn)
{
    switch (btn) {
        case BUTTON_SET1: return PRESET_1;
        case BUTTON_SET2: return PRESET_2;
        case BUTTON_SET3: return PRESET_3;
        default:          return PRESET_1; /* не должно вызываться для других кнопок */
    }
}

/**
 * @brief Записать target в Settings (клампится там) и сразу продублировать
 *        в State для PID — единственная точка входа для этой пары записей.
 */
static void apply_target_and_sync(channel_id_t ch, int32_t requested_target)
{
    if (requested_target < 0) requested_target = 0;
    Settings_SetTarget(ch, (uint16_t)requested_target);
    uint16_t clamped = Settings_GetTarget(ch); /* реальное значение после клампа диапазона 50..450 */
    State_SetSetpointTemp(ch, FIXED_FROM_INT(clamped));
}

/* Колбэки для step_accel: читают/пишут target активного канала через
 * apply_target_and_sync() (там же клампинг диапазона и синхронизация в State). */
static uint16_t accel_get(void *ctx)
{
    (void)ctx;
    return Settings_GetTarget(s_active_channel);
}

static void accel_set(void *ctx, uint16_t value)
{
    (void)ctx;
    apply_target_and_sync(s_active_channel, (int32_t)value);
}

static void accel_start(button_id_t btn)
{
    StepAccel_Start(&s_accel, btn, accel_get, accel_set, NULL, HAL_GetTick());
}

/**
 * @brief Выйти из сервисного меню в главный экран с немедленной записью
 *        настроек (не дожидаясь отложенного таймера Settings_Poll()).
 */
static void exit_service_to_main(void)
{
    Settings_Save();
    s_screen_mode = SCREEN_MODE_MAIN;
}

/**
 * @brief TOOLS (короткое): переключение активного канала.
 * @return true — событие было TOOLS и обработано (вызывающий дальше не идёт)
 */
static bool handle_tools(const button_event_t *ev)
{
    /* --- TOOLS: переключение активного канала — работает ВСЕГДА, в любом
     * режиме экрана (в т.ч. внутри сервисного меню — оно наследует активный
     * канал и TOOLS может переключить его прямо оттуда, см. menu.h).
     * Переключаем ТОЛЬКО на исправный канал. Если исправен ровно
     * один из двух — авто-фокус (см. InputFSM_Poll()) и так уже держит нас
     * на нём, так что вручную сюда попасть, будучи на исправном канале,
     * нельзя (target тогда неисправен — условие ниже ложно, кнопка
     * фактически заблокирована, штатно). Если неисправны оба —
     * переключаться тоже незачем. */
    if (ev->mask != BUTTON_MASK(BUTTON_TOOLS) || ev->type != BUTTON_EVENT_SHORT_PRESS) {
        return false;
    }

    channel_id_t target = (s_active_channel == CHANNEL_SOLDER) ? CHANNEL_DESOLDER : CHANNEL_SOLDER;
    if (!Error_IsChannelBlocked(target)) {
        s_active_channel = target;
        s_accel.active = false; /* на всякий случай, физически невозможно при живом accel, но дёшево подстраховаться */
    }
    return true;
}

/**
 * @brief Событие в режиме SCREEN_MODE_SERVICE: глобальные жесты выхода, иначе
 *        передача в Menu.
 */
static void handle_service(const button_event_t *ev)
{
    /* Глобальный выход из ЛЮБОГО уровня меню сразу в главный экран —
     * перехватывается здесь, ДО передачи в Menu (Menu эти комбинации
     * никогда не видит). Запись в EEPROM форсируется немедленно, не
     * дожидаясь обычного отложенного таймера Settings_Poll(). */
    bool global_exit =
        (ev->type == BUTTON_EVENT_CHORD_LONG && ev->mask == BUTTONS_CHORD_UP_DN_MASK) ||
        (ev->type == BUTTON_EVENT_SHORT_PRESS &&
         (ev->mask == BUTTON_MASK(BUTTON_SET1) || ev->mask == BUTTON_MASK(BUTTON_SET3)));

    if (global_exit) {
        exit_service_to_main();
        return;
    }

    if (Menu_HandleEvent(ev) == MENU_ACTION_EXIT_TO_MAIN) {
        /* Пункт "Выход" уровня User — тот же форсированный Settings_Save() */
        exit_service_to_main();
    }
}

/**
 * @brief Заблокирован ли ввод на главном экране для активного канала.
 * @return true — событие нужно отбросить
 */
static bool is_main_input_blocked(const button_event_t *ev)
{
    /* Активный канал неисправен — управление (SET/UP/DN) заблокировано,
     * TOOLS (уже обработан выше) и UP+DN аккорд (выключение/сервисное меню)
     * под этот блок не подпадают. */
    if (Error_IsChannelBlocked(s_active_channel) && ev->mask != BUTTONS_CHORD_UP_DN_MASK) {
        return true;
    }

    /* Канал выключен вручную (аккорд UP+DN) - понимает ТОЛЬКО аккорд (включение):
     * SET1/2/3 (и применение, и запись пресета) и UP/DN не действуют. Фокус на
     * такой канал передавать МОЖНО (TOOLS обработан выше) - иначе его нельзя
     * было бы снова включить. Неподключенный/неисправный канал отсечён блоком
     * выше, там же TOOLS не передаёт на него фокус. */
    if (!State_IsEnabled(s_active_channel) && ev->mask != BUTTONS_CHORD_UP_DN_MASK) {
        return true;
    }

    return false;
}

static bool is_preset_button_mask(uint8_t mask)
{
    return mask == BUTTON_MASK(BUTTON_SET1) ||
           mask == BUTTON_MASK(BUTTON_SET2) ||
           mask == BUTTON_MASK(BUTTON_SET3);
}

static bool is_up_dn_button_mask(uint8_t mask)
{
    return mask == BUTTON_MASK(BUTTON_UP) || mask == BUTTON_MASK(BUTTON_DN);
}

/**
 * @brief SET1/SET2/SET3: короткое — применить пресет, длинное — записать в
 *        пресет отображаемую температуру.
 */
static void handle_preset_button(const button_event_t *ev)
{
    button_id_t btn = (ev->mask == BUTTON_MASK(BUTTON_SET1)) ? BUTTON_SET1 :
                       (ev->mask == BUTTON_MASK(BUTTON_SET2)) ? BUTTON_SET2 : BUTTON_SET3;
    preset_id_t preset = preset_for_button(btn);

    if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
        apply_target_and_sync(s_active_channel, Settings_GetPreset(s_active_channel, preset));
    } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
        /* Пресет наследует ОТОБРАЖАЕМУЮ температуру (после гистерезиса
         * экрана), а не сырую State: иначе записанное число не совпало бы с
         * тем, что пользователь видел на экране. Числа на экране нет -
         * сохранять нечего (сюда не должно доходить: выключенный,
         * неподключенный и неисправный каналы отсечены выше). */
        int32_t shown_int;
        if (Screen_GetShownTemp(s_active_channel, &shown_int)) {
            if (shown_int < 0) shown_int = 0; /* защита от аномального/отрицательного чтения датчика */
            Settings_SetPreset(s_active_channel, preset, (uint16_t)shown_int); /* клампится 50..450 внутри */
        }
    }
}

/**
 * @brief UP/DN одиночные (не аккорд): короткое — шаг на 1, длинное — авто-повтор.
 */
static void handle_up_dn_button(const button_event_t *ev)
{
    button_id_t btn = (ev->mask == BUTTON_MASK(BUTTON_UP)) ? BUTTON_UP : BUTTON_DN;
    int32_t sign = (btn == BUTTON_UP) ? 1 : -1;

    if (ev->type == BUTTON_EVENT_SHORT_PRESS) {
        StepAccel_ApplyDelta(accel_get, accel_set, NULL, sign, 1);
    } else if (ev->type == BUTTON_EVENT_LONG_PRESS) {
        accel_start(btn);
    }
}

/**
 * @brief Аккорд UP+DN: короткий — вкл/выкл активного канала, длинный — вход
 *        в сервисное меню.
 */
static void handle_chord(const button_event_t *ev)
{
    if (ev->type == BUTTON_EVENT_CHORD_SHORT) {
        bool new_enabled = !State_IsEnabled(s_active_channel);
        State_SetEnabled(s_active_channel, new_enabled);
        if (new_enabled) {
            /* Включение канала — считаем это активностью и для Sleep,
             * иначе включённый инструмент может тут же оказаться в
             * PRESLEEP/SLEEP, если простаивал ещё до включения. */
            Sleep_ForceAwake(s_active_channel);
        }
    } else if (ev->type == BUTTON_EVENT_CHORD_LONG) {
        s_screen_mode = SCREEN_MODE_SERVICE;
        Menu_Init(); /* всегда с чистого состояния: уровень User, курсор на первом пункте */
    }
}

/**
 * @brief Разобрать одно событие от Buttons и применить действие
 *
 * Порядок проверок важен: TOOLS (любой режим) → сервисное меню → блокировка
 * ввода на главном экране → SET1/2/3 → UP/DN → аккорд UP+DN.
 * BUTTON_EVENT_VIOLATION и прочее — игнорируем молча, см. докстринг.
 */
static void dispatch_event(const button_event_t *ev)
{
    if (handle_tools(ev)) {
        return;
    }

    if (s_screen_mode == SCREEN_MODE_SERVICE) {
        handle_service(ev);
        return;
    }

    if (is_main_input_blocked(ev)) {
        return;
    }

    if (is_preset_button_mask(ev->mask)) {
        handle_preset_button(ev);
    } else if (is_up_dn_button_mask(ev->mask)) {
        handle_up_dn_button(ev);
    } else if (ev->mask == BUTTONS_CHORD_UP_DN_MASK) {
        handle_chord(ev);
    }
}

/* ---- Публичный API ---- */

void InputFSM_Init(void)
{
    s_active_channel = CHANNEL_SOLDER;
    s_screen_mode = SCREEN_MODE_MAIN;

    StepAccel_Stop(&s_accel);
}

void InputFSM_SyncStateFromSettings(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint16_t target = Settings_GetTarget((channel_id_t)ch);
        State_SetSetpointTemp((channel_id_t)ch, FIXED_FROM_INT(target));
    }
}

void InputFSM_Poll(void)
{
    /* Авто-фокус: если активный канал стал неисправен/не подключен, а
     * АЛЬТЕРНАТИВНЫЙ канал исправен — переносим фокус на него сами, не
     * дожидаясь нажатия TOOLS. Работает в любом режиме экрана —
     * тем же обоснованием, что и ручной TOOLS выше (сервисное меню
     * наследует активный канал). Если неисправны оба — условие ниже не
     * сработает ни для одной стороны (alternate тоже блокирован), фокус
     * остаётся как есть; ручной TOOLS в этом случае тоже ничего не делает
     * (см. handle_tools()) — переключаться между двумя одинаково
     * нерабочими каналами незачем. Выполняется ДО разбора очереди кнопок,
     * чтобы TOOLS в этом же тике уже видел актуальный s_active_channel. */
    if (Error_IsChannelBlocked(s_active_channel)) {
        channel_id_t alternate = (s_active_channel == CHANNEL_SOLDER) ? CHANNEL_DESOLDER : CHANNEL_SOLDER;
        if (!Error_IsChannelBlocked(alternate)) {
            s_active_channel = alternate;
            s_accel.active = false; /* см. тот же комментарий у TOOLS в handle_tools() */
        }
    }

    button_event_t ev;
    while (Buttons_PopEvent(&ev)) {
        dispatch_event(&ev);
    }

    if (s_screen_mode == SCREEN_MODE_SERVICE) {
        Menu_Poll();
        return; /* авто-повтор UP/DN главного экрана (ниже) не имеет смысла в меню */
    }

    if (s_accel.active) {
        if (!Buttons_IsHeld(s_accel.button) || Error_IsChannelBlocked(s_active_channel) || !State_IsEnabled(s_active_channel)) {
            s_accel.active = false;
        } else {
            StepAccel_Tick(&s_accel, HAL_GetTick());
        }
    }
}

channel_id_t InputFSM_GetActiveChannel(void)
{
    return s_active_channel;
}

screen_mode_t InputFSM_GetScreenMode(void)
{
    return s_screen_mode;
}
