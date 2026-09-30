/**
 * @file sleep.c
 * @brief Реализация sleep.h — дебаунс сырых входов + таймерная FSM на канал.
 *
 * Таймеры PreSleep/Sleep ПОСЛЕДОВАТЕЛЬНЫ (двухступенчатые), а не
 * независимы: PreSleepTimeout отсчитывается от начала простоя
 * (idle_start_tick). SleepTimeout вступает в игру только ПОСЛЕ того, как
 * отработал PreSleepTimeout — отсчитывается от момента входа в PRESLEEP,
 * а не от начала простоя — либо сразу от начала простоя, если
 * PreSleepTimeout==0 (выключен). Если PreSleepTimeout>0, а SleepTimeout==0
 * — канал зависает в PRESLEEP бессрочно (SleepTimeout выключен).
 *
 * Пока инструмент не подключен (Error_IsChannelIdle()), канал не опрашивается
 * вовсе и остаётся в AWAKE без таймера — см. начало poll_channel().
 */

#include "sleep.h"
#include "settings.h"
#include "error.h" /* Error_IsChannelIdle() — "инструмент не подключен", см. poll_channel() */
#include "state.h" /* State_IsEnabled() — гейт для Beep_EnteredSleep(), см. notify_if_entered_sleep() */
#include "beep.h"
#include "main.h" /* Dock_Pin/Dock_GPIO_Port, Btn_Pump_Pin/Btn_Pump_GPIO_Port */
#include "stm32f4xx_hal.h"

typedef struct {
    sleep_mode_t mode;

    /* Дебаунс сырого уровня "простаивает" (тот же принцип, что в buttons.c) */
    bool     debounced_idle;
    bool     raw_idle_candidate;
    uint8_t  stable_count;

    /* Единый момент начала простоя — от него отсчитывается PreSleepTimeout
     * (и SleepTimeout, если PreSleepTimeout==0 — см. poll_channel()); при
     * работающем PreSleepTimeout SleepTimeout отсчитывается уже не от
     * этой точки, а от момента входа в PRESLEEP (последовательно).
     * оба порога (PreSleepTimeout и SleepTimeout). 0 = не простаивает.
     * HAL_GetTick()==0 в первую миллисекунду после старта МК теоретически
     * не отличим от "не идёт" — не критично (окно в 1 мс, канал стартует
     * в AWAKE, см. Sleep_Init()). */
    uint32_t idle_start_tick;
} sleep_channel_t;

static sleep_channel_t s_channels[CHANNEL_COUNT];

/** @brief Сырой (недебаунсенный) уровень "простаивает" для канала.
 *  Дебаунс — ниже, в poll_channel(), намеренно: дребезг не должен сбрасывать
 *  таймер простоя. Тот же вход Btn_Pump читает Pump (pump.c) СЫРЫМ уровнем,
 *  без дебаунса — там нужна мгновенная реакция; эти два контракта не
 *  унифицируются (см. pump.h).
 *  Оба входа GPIO_PULLUP, но реальная полярность разная (проверено на
 *  железе — паяльник и отсос физически разные датчики, симметрии нет):
 *  Dock==1        -> паяльник В подставке          -> простой (idle=true)
 *  Btn_Pump==1    -> кнопка ОТПУЩЕНА, помпа не работает -> простой (idle=true)
 *  Btn_Pump==0    -> кнопка НАЖАТА, помпа работает      -> занят (idle=false) */
static bool read_raw_idle(channel_id_t ch)
{
    if (ch == CHANNEL_SOLDER) {
        return HAL_GPIO_ReadPin(Dock_GPIO_Port, Dock_Pin) == GPIO_PIN_SET;
    } else {
        return HAL_GPIO_ReadPin(Btn_Pump_GPIO_Port, Btn_Pump_Pin) == GPIO_PIN_SET;
    }
}

static void stop_reset_timer(sleep_channel_t *c)
{
    c->idle_start_tick = 0;
    c->mode = SLEEP_MODE_AWAKE;
}

/** @brief Сигнал (Beep_EnteredSleep()) ровно на фронте входа в SLEEP —
 *        не на каждом опросе, пока канал уже в SLEEP. Только для
 *        ВКЛЮЧЁННОГО канала: если канал выключен аккордом (State_IsEnabled
 *        false), нагрева и так уже нет, и сигнал о "сне" был бы ложным
 *        поводом — таймер Sleep при этом продолжает считать (не привязан
 *        к State, см. докстринг файла), просто сигналить об этом незачем. */
static void notify_if_entered_sleep(channel_id_t ch, sleep_mode_t prev_mode, sleep_mode_t new_mode)
{
    if (new_mode == SLEEP_MODE_SLEEP && prev_mode != SLEEP_MODE_SLEEP && State_IsEnabled(ch)) {
        Beep_EnteredSleep();
    }
}

void Sleep_Init(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        sleep_channel_t *c = &s_channels[i];
        c->debounced_idle      = false;
        c->raw_idle_candidate  = false;
        c->stable_count        = 0;
        stop_reset_timer(c);
    }
}

static void poll_channel(channel_id_t ch)
{
    sleep_channel_t *c = &s_channels[ch];

    /* ---- Инструмент не подключен: всё начинается с подключения ----
     * Пока Error_IsChannelIdle() — состояние Dock/Btn_Pump ничего не значит
     * (Dock подтянут к питанию и без паяльника читается как "в подставке"),
     * поэтому его не читаем и не дебаунсим, а всё состояние канала держим
     * сброшенным. Так таймер простоя, режимы PRESLEEP/SLEEP и сигнал
     * Beep_EnteredSleep() не могут возникнуть у отсутствующего инструмента,
     * а после подключения дебаунс и отсчёт простоя начинаются с нуля
     * (обычный фронт "занят -> простаивает" ниже). */
    if (Error_IsChannelIdle(ch)) {
        c->debounced_idle     = false;
        c->raw_idle_candidate = false;
        c->stable_count       = 0;
        stop_reset_timer(c);
        return;
    }

    /* ---- Дебаунс (см. buttons.c: N стабильных опросов подряд) ---- */
    bool raw = read_raw_idle(ch);
    if (raw == c->raw_idle_candidate) {
        if (c->stable_count < SLEEP_DEBOUNCE_TICKS) {
            c->stable_count++;
        }
    } else {
        c->raw_idle_candidate = raw;
        c->stable_count = 1;
    }

    bool debounced = (c->stable_count >= SLEEP_DEBOUNCE_TICKS) ? c->raw_idle_candidate
                                                                 : c->debounced_idle;

    /* ---- Переход занят -> простаивает: запускаем единый таймер простоя ---- */
    if (debounced && !c->debounced_idle) {
        c->idle_start_tick = HAL_GetTick();
        if (c->idle_start_tick == 0) c->idle_start_tick = 1; /* см. комментарий в структуре: 0 зарезервирован под "не идёт" */
        c->mode = SLEEP_MODE_AWAKE;
    }
    /* ---- Переход простаивает -> занят: сброс таймера, назад в AWAKE ---- */
    else if (!debounced && c->debounced_idle) {
        stop_reset_timer(c);
    }
    c->debounced_idle = debounced;

    if (!debounced || c->idle_start_tick == 0) {
        return; /* инструмент используется — таймеру сейчас нечего делать */
    }

    sleep_mode_t prev_mode = c->mode; /* для края AWAKE/PRESLEEP -> SLEEP ниже, см. Beep_EnteredSleep() */

    uint32_t now = HAL_GetTick();
    uint32_t elapsed_ms = now - c->idle_start_tick; /* корректно и при переполнении HAL_GetTick() */

    /* ---- Последовательная (двухступенчатая) проверка порогов ----
     * PreSleepTimeout отсчитывается от начала простоя. SleepTimeout
     * вступает в игру только ПОСЛЕ того, как отработал PreSleepTimeout
     * (отсчитывается от момента входа в PRESLEEP, а не от начала простоя) —
     * либо сразу от начала простоя, если PreSleepTimeout==0 (выключен). */
    uint16_t presleep_timeout_min = Settings_GetPreSleepTimeout(ch);
    uint16_t sleep_timeout_min = Settings_GetSleepTimeout(ch);

    if (presleep_timeout_min > 0) {
        uint32_t presleep_total_ms = (uint32_t)presleep_timeout_min * 60000UL;
        if (elapsed_ms < presleep_total_ms) {
            c->mode = SLEEP_MODE_AWAKE;
            return;
        }
        /* PreSleep уже отработал — SleepTimeout ждёт своей очереди и
         * считается от МОМЕНТА ВХОДА В PRESLEEP, а не от начала простоя. */
        if (sleep_timeout_min > 0) {
            uint32_t elapsed_since_presleep_ms = elapsed_ms - presleep_total_ms;
            uint32_t sleep_total_ms = (uint32_t)sleep_timeout_min * 60000UL;
            c->mode = (elapsed_since_presleep_ms >= sleep_total_ms) ? SLEEP_MODE_SLEEP : SLEEP_MODE_PRESLEEP;
        } else {
            c->mode = SLEEP_MODE_PRESLEEP; /* SleepTimeout выключен — зависаем в PRESLEEP бессрочно */
        }
        notify_if_entered_sleep(ch, prev_mode, c->mode);
        return;
    }

    /* PreSleepTimeout выключен — SleepTimeout считает прямо от начала простоя */
    if (sleep_timeout_min > 0) {
        uint32_t sleep_total_ms = (uint32_t)sleep_timeout_min * 60000UL;
        c->mode = (elapsed_ms >= sleep_total_ms) ? SLEEP_MODE_SLEEP : SLEEP_MODE_AWAKE;
        notify_if_entered_sleep(ch, prev_mode, c->mode);
        return;
    }

    c->mode = SLEEP_MODE_AWAKE; /* оба порога выключены */
}

void Sleep_Poll(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        poll_channel((channel_id_t)ch);
    }
}

sleep_mode_t Sleep_GetMode(channel_id_t ch)
{
    if (!channel_valid(ch)) return SLEEP_MODE_AWAKE;
    return s_channels[ch].mode;
}

void Sleep_ForceAwake(channel_id_t ch)
{
    if (!channel_valid(ch)) return;
    sleep_channel_t *c = &s_channels[ch];

    c->mode = SLEEP_MODE_AWAKE;
    if (c->debounced_idle) {
        /* Сырой вход физически всё ещё "простой" (например, отсос
         * включили аккордом, а помпа при этом не нажата) — обычный
         * фронт занят->простаивает в poll_channel() тут не сработает,
         * т.к. debounced_idle уже true и не меняется. Перезапускаем
         * таймер вручную от текущего момента. */
        c->idle_start_tick = HAL_GetTick();
        if (c->idle_start_tick == 0) c->idle_start_tick = 1; /* см. комментарий в структуре */
    } else {
        /* Вход реально не простаивает — таймеру нечего считать. */
        c->idle_start_tick = 0;
    }
}

uint32_t Sleep_GetRemainingSeconds(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    const sleep_channel_t *c = &s_channels[ch];

    if (c->idle_start_tick == 0) return 0; /* не простаивает */

    uint32_t now = HAL_GetTick();
    uint32_t elapsed_ms = now - c->idle_start_tick;
    uint16_t presleep_min = Settings_GetPreSleepTimeout(ch);
    uint16_t sleep_min = Settings_GetSleepTimeout(ch);

    if (c->mode == SLEEP_MODE_AWAKE) {
        /* До ближайшего следующего события: PreSleep, если он включён —
         * иначе (PreSleep выключен) сразу Sleep, если включён он. */
        uint32_t candidate_ms;
        if (presleep_min > 0) {
            candidate_ms = (uint32_t)presleep_min * 60000UL;
        } else if (sleep_min > 0) {
            candidate_ms = (uint32_t)sleep_min * 60000UL;
        } else {
            return 0; /* оба порога выключены */
        }
        if (elapsed_ms >= candidate_ms) return 0; /* на грани перехода — следующий Sleep_Poll() переведёт режим */
        /* Округление ВВЕРХ (не вниз): иначе на последней неполной секунде
         * до срабатывания таймера (elapsed_ms и candidate_ms отличаются
         * меньше чем на 1000мс) remaining уже становится 0 при ещё не
         * переключившемся режиме — экран трактует это как "таймер не
         * идёт" и на секунду прячет иконку+текст ДО того, как следующий
         * Sleep_Poll() (10мс) реально сменит режим. Округление вверх
         * держит remaining>=1, пока elapsed_ms < candidate_ms, и переход
         * между таймерами становится бесшовным. */
        return ((candidate_ms - elapsed_ms) + 999U) / 1000U;
    }

    if (c->mode == SLEEP_MODE_PRESLEEP) {
        /* SleepTimeout считается от момента входа в PRESLEEP, т.е. от
         * presleep_total_ms, а не от начала простоя — см. poll_channel(). */
        if (sleep_min == 0) return 0; /* выключено — останется в PRESLEEP бессрочно */
        uint32_t sleep_deadline_ms = (uint32_t)presleep_min * 60000UL + (uint32_t)sleep_min * 60000UL;
        if (elapsed_ms >= sleep_deadline_ms) return 0;
        /* Та же причина округления вверх, что и в ветке AWAKE выше — иначе
         * "Предсон" (remaining==0, см. update_sleep_status()) мигал бы на
         * последней секунде PRESLEEP вместо "0:01". */
        return ((sleep_deadline_ms - elapsed_ms) + 999U) / 1000U;
    }

    /* SLEEP_MODE_SLEEP — финальный режим, считать больше нечего */
    return 0;
}
