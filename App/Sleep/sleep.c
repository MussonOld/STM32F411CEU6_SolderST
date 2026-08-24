/**
 * @file sleep.c
 * @brief Реализация sleep.h — дебаунс сырых входов + двухуровневая
 *        таймерная FSM на канал.
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

    /* Таймеры — храним отметку старта, а не остаток; 0 = таймер не идёт.
     * HAL_GetTick() ==0 в первую миллисекунду после старта МК теоретически
     * не отличим от "не идёт" — не критично (окно в 1 мс, канал стартует
     * в AWAKE без запущенных таймеров, см. Sleep_Init()). */
    uint32_t timer1_start_tick;
    uint32_t timer2_start_tick;
} sleep_channel_t;

static sleep_channel_t s_channels[CHANNEL_COUNT];

static inline bool channel_valid(channel_id_t ch)
{
    return (ch >= 0) && (ch < CHANNEL_COUNT);
}

/** @brief Сырой (недебаунсенный) уровень "простаивает" для канала.
 *  Оба входа активный низкий (GPIO_PULLUP), НО смысл разный:
 *  Dock==0     -> паяльник В подставке   -> простой (idle=true)
 *  Btn_Pump==0 -> кнопка НАЖАТА, помпа работает -> занят, НЕ простой (idle=false) */
static bool read_raw_idle(channel_id_t ch)
{
    if (ch == CHANNEL_SOLDER) {
        return HAL_GPIO_ReadPin(Dock_GPIO_Port, Dock_Pin) == GPIO_PIN_RESET;
    } else {
        return HAL_GPIO_ReadPin(Btn_Pump_GPIO_Port, Btn_Pump_Pin) == GPIO_PIN_SET;
    }
}

static void stop_reset_timers(sleep_channel_t *c)
{
    c->timer1_start_tick = 0;
    c->timer2_start_tick = 0;
    c->mode = SLEEP_MODE_AWAKE;
}

void Sleep_Init(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        sleep_channel_t *c = &s_channels[i];
        c->debounced_idle      = false;
        c->raw_idle_candidate  = false;
        c->stable_count        = 0;
        stop_reset_timers(c);
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

    /* ---- Переход занят -> простаивает: запускаем Таймер 1 ---- */
    if (debounced && !c->debounced_idle) {
        c->timer1_start_tick = HAL_GetTick();
        if (c->timer1_start_tick == 0) c->timer1_start_tick = 1; /* см. комментарий в структуре: 0 зарезервирован под "не идёт" */
        c->timer2_start_tick = 0;
        c->mode = SLEEP_MODE_AWAKE;
    }
    /* ---- Переход простаивает -> занят: сброс обоих таймеров, назад в AWAKE ---- */
    else if (!debounced && c->debounced_idle) {
        stop_reset_timers(c);
    }
    c->debounced_idle = debounced;

    if (!debounced) {
        return; /* инструмент используется — таймерам сейчас нечего делать */
    }

    uint32_t now = HAL_GetTick();

    /* ---- Таймер 1: AWAKE -> PRESLEEP ---- */
    if (c->mode == SLEEP_MODE_AWAKE && c->timer1_start_tick != 0) {
        uint16_t timeout_min = Settings_GetPreSleepTimeout(ch);
        if (timeout_min > 0) {
            uint32_t elapsed_ms = now - c->timer1_start_tick; /* корректно и при переполнении HAL_GetTick() */
            if (elapsed_ms >= (uint32_t)timeout_min * 60000UL) {
                c->mode = SLEEP_MODE_PRESLEEP;
                c->timer2_start_tick = now;
                if (c->timer2_start_tick == 0) c->timer2_start_tick = 1;
            }
        }
    }

    /* ---- Таймер 2: PRESLEEP -> SLEEP ---- */
    if (c->mode == SLEEP_MODE_PRESLEEP && c->timer2_start_tick != 0) {
        uint16_t timeout_min = Settings_GetSleepTimeout(ch);
        if (timeout_min > 0) {
            uint32_t elapsed_ms = now - c->timer2_start_tick;
            if (elapsed_ms >= (uint32_t)timeout_min * 60000UL) {
                c->mode = SLEEP_MODE_SLEEP;
            }
        }
    }
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
