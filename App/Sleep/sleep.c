/**
 * @file sleep.c
 * @brief Реализация sleep.h — дебаунс сырых входов + таймерная FSM на канал.
 *
 * Таймеры PreSleep/Sleep НЕЗАВИСИМЫ друг от друга: оба отсчитываются от
 * ОДНОГО И ТОГО ЖЕ момента начала простоя (idle_start_tick), а не один
 * от другого. Режим на каждом опросе выбирается прямым сравнением
 * накопленного времени простоя с обоими порогами — SLEEP проверяется
 * первым, чтобы он "победил", даже если SleepTimeout < PreSleepTimeout
 * или если PreSleepTimeout==0 (выключен) — тогда PRESLEEP просто
 * пропускается, канал уходит из AWAKE сразу в SLEEP по истечении
 * SleepTimeout. Симметрично: PreSleepTimeout может быть > 0 при
 * SleepTimeout==0 — тогда канал зависает в PRESLEEP бессрочно, как и
 * раньше.
 */

#include "sleep.h"
#include "settings.h"
#include "main.h" /* Dock_Pin/Dock_GPIO_Port, Btn_Pump_Pin/Btn_Pump_GPIO_Port */
#include "stm32f4xx_hal.h"

typedef struct {
    sleep_mode_t mode;

    /* Дебаунс сырого уровня "простаивает" (тот же принцип, что в buttons.c) */
    bool     debounced_idle;
    bool     raw_idle_candidate;
    uint8_t  stable_count;

    /* Единый момент начала простоя — от него независимо отсчитываются
     * оба порога (PreSleepTimeout и SleepTimeout). 0 = не простаивает.
     * HAL_GetTick()==0 в первую миллисекунду после старта МК теоретически
     * не отличим от "не идёт" — не критично (окно в 1 мс, канал стартует
     * в AWAKE, см. Sleep_Init()). */
    uint32_t idle_start_tick;
} sleep_channel_t;

static sleep_channel_t s_channels[CHANNEL_COUNT];

static inline bool channel_valid(channel_id_t ch)
{
    return (ch >= 0) && (ch < CHANNEL_COUNT);
}

/** @brief Сырой (недебаунсенный) уровень "простаивает" для канала.
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

    uint32_t now = HAL_GetTick();
    uint32_t elapsed_ms = now - c->idle_start_tick; /* корректно и при переполнении HAL_GetTick() */

    /* ---- Независимая проверка обоих порогов от ОДНОГО старта простоя ----
     * SLEEP проверяется первым: если SleepTimeout короче (или PreSleepTimeout
     * выключен), канал должен уйти в SLEEP напрямую из AWAKE, минуя PRESLEEP. */
    uint16_t sleep_timeout_min = Settings_GetSleepTimeout(ch);
    if (sleep_timeout_min > 0) {
        uint32_t sleep_total_ms = (uint32_t)sleep_timeout_min * 60000UL;
        if (elapsed_ms >= sleep_total_ms) {
            c->mode = SLEEP_MODE_SLEEP;
            return;
        }
    }

    uint16_t presleep_timeout_min = Settings_GetPreSleepTimeout(ch);
    if (presleep_timeout_min > 0) {
        uint32_t presleep_total_ms = (uint32_t)presleep_timeout_min * 60000UL;
        if (elapsed_ms >= presleep_total_ms) {
            c->mode = SLEEP_MODE_PRESLEEP;
            return;
        }
    }

    c->mode = SLEEP_MODE_AWAKE;
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

uint32_t Sleep_GetRemainingSeconds(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    const sleep_channel_t *c = &s_channels[ch];

    if (c->idle_start_tick == 0) return 0; /* не простаивает */

    uint32_t now = HAL_GetTick();
    uint32_t elapsed_ms = now - c->idle_start_tick;

    if (c->mode == SLEEP_MODE_AWAKE) {
        /* До ближайшего порога — который из двух наступит раньше и включён (>0) */
        uint16_t sleep_min = Settings_GetSleepTimeout(ch);
        uint16_t presleep_min = Settings_GetPreSleepTimeout(ch);

        uint32_t candidate_ms = 0;
        bool have_candidate = false;

        if (sleep_min > 0) {
            candidate_ms = (uint32_t)sleep_min * 60000UL;
            have_candidate = true;
        }
        if (presleep_min > 0) {
            uint32_t presleep_ms = (uint32_t)presleep_min * 60000UL;
            if (!have_candidate || presleep_ms < candidate_ms) {
                candidate_ms = presleep_ms;
                have_candidate = true;
            }
        }

        if (!have_candidate) return 0; /* оба порога выключены */
        if (elapsed_ms >= candidate_ms) return 0; /* на грани перехода — следующий Sleep_Poll() переведёт режим */
        return (candidate_ms - elapsed_ms) / 1000U;
    }

    if (c->mode == SLEEP_MODE_PRESLEEP) {
        uint16_t sleep_min = Settings_GetSleepTimeout(ch);
        if (sleep_min == 0) return 0; /* выключено — останется в PRESLEEP бессрочно */
        uint32_t total_ms = (uint32_t)sleep_min * 60000UL;
        if (elapsed_ms >= total_ms) return 0;
        return (total_ms - elapsed_ms) / 1000U;
    }

    /* SLEEP_MODE_SLEEP — финальный режим, считать больше нечего */
    return 0;
}
