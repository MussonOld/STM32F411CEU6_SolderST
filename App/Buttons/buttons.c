/**
 * @file buttons.c
 * @brief Реализация buttons.h — см. правила в шапке заголовка.
 *
 * Пины (активный уровень низкий, GPIO_PULLUP в CubeMX):
 * SET1=PA8, SET2=PA9, SET3=PA10, DN=PA11, UP=PA12, TOOLS=PA15.
 */

#include "buttons.h"
#include "stm32f4xx_hal.h"
#include "main.h"

#define BUTTONS_EVENT_QUEUE_SIZE  8U

/* ---- Таблица пинов, индекс == button_id_t ---- */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
} button_pin_t;

static const button_pin_t s_pins[BUTTON_COUNT] = {
    [BUTTON_SET1]  = { SET1_GPIO_Port,  SET1_Pin  },
    [BUTTON_SET2]  = { SET2_GPIO_Port,  SET2_Pin  },
    [BUTTON_SET3]  = { SET3_GPIO_Port,  SET3_Pin  },
    [BUTTON_DN]    = { DN_GPIO_Port,    DN_Pin    },
    [BUTTON_UP]    = { UP_GPIO_Port,    UP_Pin    },
    [BUTTON_TOOLS] = { TOOLS_GPIO_Port, TOOLS_Pin },
};

/* ---- Антидребезг, по кнопке ---- */
static bool     s_candidate[BUTTON_COUNT];   /* последний сырой уровень (true = нажата) */
static uint8_t  s_stable_count[BUTTON_COUNT]; /* сколько опросов подряд candidate не менялся */
static bool     s_confirmed[BUTTON_COUNT];   /* продебонсенное состояние */

/* ---- Очередь событий (кольцевой буфер, без malloc) ---- */
static button_event_t s_queue[BUTTONS_EVENT_QUEUE_SIZE];
static uint8_t s_queue_head = 0;
static uint8_t s_queue_tail = 0;
static uint8_t s_queue_count = 0;

/* ---- Состояние эпизода (episode = непрерывный отрезок "хоть одна кнопка нажата") ---- */
typedef enum {
    EPISODE_NONE,
    EPISODE_PENDING_CHORD, /* нажата только UP или только DN, ждём партнёра в окне */
    EPISODE_SOLO,
    EPISODE_CHORD,
    EPISODE_VIOLATION
} episode_state_t;

static episode_state_t s_episode = EPISODE_NONE;
static uint32_t s_first_down_tick = 0;
static uint8_t  s_active_mask = 0;   /* для SOLO — один бит; для CHORD — BUTTONS_CHORD_UP_DN_MASK */
static bool     s_long_fired = false;
static bool     s_ignore_flag = false;

/* ---- Внутренние примитивы ---- */

static void push_event(button_event_type_t type, uint8_t mask)
{
    if (s_queue_count >= BUTTONS_EVENT_QUEUE_SIZE) {
        return; /* очередь переполнена — событие теряется, не блокируем поток */
    }
    s_queue[s_queue_tail].type = type;
    s_queue[s_queue_tail].mask = mask;
    s_queue_tail = (uint8_t)((s_queue_tail + 1) % BUTTONS_EVENT_QUEUE_SIZE);
    s_queue_count++;
}

static uint8_t popcount8(uint8_t v)
{
    uint8_t count = 0;
    while (v) {
        count += (v & 1U);
        v >>= 1;
    }
    return count;
}

static void raise_violation(uint8_t mask_snapshot)
{
    s_episode = EPISODE_VIOLATION;
    s_ignore_flag = true;
    push_event(BUTTON_EVENT_VIOLATION, mask_snapshot);
}

/**
 * @brief Обновить антидребезг всех кнопок, вернуть подтверждённую маску
 */
static uint8_t update_debounce(void)
{
    uint8_t mask = 0;

    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        bool raw = (HAL_GPIO_ReadPin(s_pins[i].port, s_pins[i].pin) == GPIO_PIN_RESET); /* активный низкий */

        if (raw == s_candidate[i]) {
            if (s_stable_count[i] < BUTTONS_DEBOUNCE_TICKS) {
                s_stable_count[i]++;
            }
            if (s_stable_count[i] >= BUTTONS_DEBOUNCE_TICKS) {
                s_confirmed[i] = raw;
            }
        } else {
            s_candidate[i] = raw;
            s_stable_count[i] = 1;
        }

        if (s_confirmed[i]) {
            mask |= BUTTON_MASK(i);
        }
    }

    return mask;
}

/**
 * @brief Финализировать эпизод по полному отпусканию (down_mask == 0)
 */
static void finalize_episode(void)
{
    switch (s_episode) {
        case EPISODE_SOLO:
            if (!s_long_fired) {
                push_event(BUTTON_EVENT_SHORT_PRESS, s_active_mask);
            }
            break;

        case EPISODE_CHORD:
            if (!s_long_fired) {
                push_event(BUTTON_EVENT_CHORD_SHORT, s_active_mask);
            }
            break;

        case EPISODE_VIOLATION:
            s_ignore_flag = false; /* флаг снимается только после полного отпускания всех кнопок */
            break;

        case EPISODE_PENDING_CHORD:
        case EPISODE_NONE:
        default:
            break;
    }

    s_episode = EPISODE_NONE;
    s_active_mask = 0;
    s_long_fired = false;
}

/**
 * @brief Один шаг FSM эпизода при down_mask != 0
 */
static void step_episode(uint8_t down_mask, uint32_t now)
{
    uint8_t count = popcount8(down_mask);

    if (s_episode == EPISODE_NONE) {
        /* Начало нового эпизода */
        s_first_down_tick = now;
        s_long_fired = false;

        if (count == 1) {
            uint8_t only_bit = down_mask;
            if (only_bit == BUTTON_MASK(BUTTON_UP) || only_bit == BUTTON_MASK(BUTTON_DN)) {
                s_episode = EPISODE_PENDING_CHORD;
                s_active_mask = only_bit;
            } else {
                s_episode = EPISODE_SOLO;
                s_active_mask = only_bit;
            }
        } else {
            /* Сразу 2+ кнопки в момент старта эпизода (в пределах одного
             * опроса) — уже нарушение по правилу "> 2 недопустимо" либо
             * "любая пара кроме UP+DN недопустима". */
            raise_violation(down_mask);
        }
        return;
    }

    switch (s_episode) {
        case EPISODE_PENDING_CHORD: {
            uint32_t elapsed = now - s_first_down_tick;

            if (count == 1) {
                if (elapsed > BUTTONS_CHORD_WINDOW_MS) {
                    /* партнёр не подошёл вовремя — это обычное одиночное
                     * нажатие UP или DN, дальше живёт как EPISODE_SOLO */
                    s_episode = EPISODE_SOLO;
                }
            } else if (count == 2) {
                if (down_mask == BUTTONS_CHORD_UP_DN_MASK) {
                    if (elapsed <= BUTTONS_CHORD_WINDOW_MS) {
                        s_episode = EPISODE_CHORD;
                        s_active_mask = BUTTONS_CHORD_UP_DN_MASK;
                    } else {
                        /* партнёр подошёл, но окно уже вышло — нарушение */
                        raise_violation(down_mask);
                    }
                } else {
                    /* пара кнопок, но не UP+DN — нарушение */
                    raise_violation(down_mask);
                }
            } else {
                /* 3+ кнопки — нарушение */
                raise_violation(down_mask);
            }
            break;
        }

        case EPISODE_SOLO: {
            if (count > 1) {
                /* к одиночной кнопке присоединилась ещё одна — нарушение */
                raise_violation(down_mask);
                break;
            }

            uint32_t elapsed = now - s_first_down_tick;

            if (s_active_mask == BUTTON_MASK(BUTTON_TOOLS)) {
                /* TOOLS — только короткое нажатие; удержание дольше порога
                 * само по себе нарушение, long-событие для неё не бывает */
                if (elapsed >= BUTTONS_LONG_PRESS_MS) {
                    raise_violation(down_mask);
                }
            } else if (elapsed >= BUTTONS_LONG_PRESS_MS && !s_long_fired) {
                push_event(BUTTON_EVENT_LONG_PRESS, s_active_mask);
                s_long_fired = true;
            }
            break;
        }

        case EPISODE_CHORD: {
            /* Легитимно: down_mask — это UP, DN, либо UP|DN (частичное
             * отпускание аккорда в процессе). Нарушение — только если
             * появился бит вне пары UP/DN. */
            if ((down_mask & (uint8_t)~BUTTONS_CHORD_UP_DN_MASK) != 0) {
                raise_violation(down_mask);
                break;
            }

            if (down_mask == BUTTONS_CHORD_UP_DN_MASK) {
                uint32_t elapsed = now - s_first_down_tick;
                if (elapsed >= BUTTONS_LONG_PRESS_MS && !s_long_fired) {
                    push_event(BUTTON_EVENT_CHORD_LONG, s_active_mask);
                    s_long_fired = true;
                }
            }
            /* если down_mask — только один бит из пары, обе кнопки ещё не
             * отпущены полностью: ждём down_mask == 0 в finalize_episode() */
            break;
        }

        case EPISODE_VIOLATION:
            /* ничего не делаем — ждём полного отпускания */
            break;

        case EPISODE_NONE:
        default:
            break;
    }
}

/* ---- Публичный API ---- */

void Buttons_Init(void)
{
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
        bool raw = (HAL_GPIO_ReadPin(s_pins[i].port, s_pins[i].pin) == GPIO_PIN_RESET);
        s_candidate[i]    = raw;
        s_stable_count[i] = BUTTONS_DEBOUNCE_TICKS; /* сразу считаем стартовое состояние подтверждённым */
        s_confirmed[i]    = raw;
    }

    s_queue_head = 0;
    s_queue_tail = 0;
    s_queue_count = 0;

    s_episode = EPISODE_NONE;
    s_active_mask = 0;
    s_long_fired = false;
    s_ignore_flag = false;
}

void Buttons_Poll(void)
{
    uint32_t now = HAL_GetTick();
    uint8_t down_mask = update_debounce();

    if (down_mask == 0) {
        if (s_episode != EPISODE_NONE) {
            finalize_episode();
        }
    } else {
        step_episode(down_mask, now);
    }
}

bool Buttons_PopEvent(button_event_t *event)
{
    if (s_queue_count == 0 || event == NULL) {
        return false;
    }

    *event = s_queue[s_queue_head];
    s_queue_head = (uint8_t)((s_queue_head + 1) % BUTTONS_EVENT_QUEUE_SIZE);
    s_queue_count--;
    return true;
}

bool Buttons_IsHeld(button_id_t id)
{
    if (id >= BUTTON_COUNT) {
        return false;
    }
    return s_confirmed[id];
}

bool Buttons_IsIgnored(void)
{
    return s_ignore_flag;
}
