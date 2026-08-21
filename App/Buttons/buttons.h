/**
 * @file buttons.h
 * @brief FSM обработки 6 кнопок станции: короткие/длинные нажатия,
 *        единственный разрешённый аккорд UP+DN, детектор нарушений правил ввода.
 *
 * Правила (зафиксированы, см. описание проекта):
 *  - Более 2 кнопок одновременно — всегда нарушение.
 *  - Единственный разрешённый аккорд — UP+DN. Чтобы он сформировался, вторая
 *    кнопка пары должна быть нажата в течение BUTTONS_CHORD_WINDOW_MS после
 *    первой. Более поздний "довесок" — нарушение, а не тихо игнорируется.
 *  - Любая другая пара/тройка кнопок — нарушение.
 *  - TOOLS — только короткое нажатие. Долгое удержание TOOLS и любой аккорд
 *    с её участием — нарушение.
 *  - Флаг нарушения снимается только после полного отпускания ВСЕХ кнопок
 *    (Buttons_IsIgnored() держит true, пока хоть одна кнопка ещё зажата).
 *
 * Драйвер отдаёт два независимых канала информации наверх:
 *  1. Дискретные события (см. button_event_t) — очередь, вычитывается через
 *     Buttons_PopEvent(). Формируются по правилам выше.
 *  2. Сырое (но продебонсенное) состояние "кнопка сейчас удержана" —
 *     Buttons_IsHeld(). Не зависит от классификации short/long/chord и от
 *     флага нарушения — нужно для функций вида "пока кнопка нажата, работает
 *     насос" (Pump_ON), решение о такой политике — уровнем выше, в таблице
 *     действий.
 *
 * Buttons_Poll() должна вызываться из главного цикла с постоянным периодом
 * ~BUTTONS_POLL_MS (не из ISR). Не блокирует, не использует HAL_Delay.
 */

#ifndef BUTTONS_H
#define BUTTONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* ---- Тайминги (подбираются, значения по умолчанию — см. .c) ---- */
#define BUTTONS_POLL_MS          10U   /* ожидаемый период вызова Buttons_Poll() */
#define BUTTONS_DEBOUNCE_TICKS    3U   /* подряд стабильных опросов для подтверждения уровня */
#define BUTTONS_CHORD_WINDOW_MS  40U   /* окно на "довесок" второй кнопки аккорда UP+DN */
#define BUTTONS_LONG_PRESS_MS   600U   /* порог короткое/длинное */

/**
 * @brief Идентификаторы кнопок. Соответствуют подписям CubeMX на схеме.
 */
typedef enum {
    BUTTON_SET1 = 0,   /* PA8  */
    BUTTON_SET2,       /* PA9  */
    BUTTON_SET3,       /* PA10 */
    BUTTON_DN,         /* PA11 */
    BUTTON_UP,         /* PA12 */
    BUTTON_TOOLS,      /* PA15 */
    BUTTON_COUNT
} button_id_t;

/** @brief Битовая маска одной кнопки */
#define BUTTON_MASK(id)  ((uint8_t)(1u << (id)))

/** @brief Маска единственного разрешённого аккорда */
#define BUTTONS_CHORD_UP_DN_MASK  ((uint8_t)(BUTTON_MASK(BUTTON_UP) | BUTTON_MASK(BUTTON_DN)))

/**
 * @brief Тип дискретного события
 */
typedef enum {
    BUTTON_EVENT_SHORT_PRESS,  /**< Одна кнопка, короткое нажатие. mask — один бит. */
    BUTTON_EVENT_LONG_PRESS,   /**< Одна кнопка, длинное нажатие (TOOLS никогда). mask — один бит. */
    BUTTON_EVENT_CHORD_SHORT,  /**< UP+DN, короткое. mask == BUTTONS_CHORD_UP_DN_MASK. */
    BUTTON_EVENT_CHORD_LONG,   /**< UP+DN, длинное. mask == BUTTONS_CHORD_UP_DN_MASK. */
    BUTTON_EVENT_VIOLATION     /**< Нарушение правил ввода. mask — снимок кнопок на момент обнаружения. */
} button_event_type_t;

/**
 * @brief Дискретное событие ввода
 */
typedef struct {
    button_event_type_t type;
    uint8_t mask;
} button_event_t;

/**
 * @brief Инициализировать драйвер (сбросить внутреннее состояние FSM)
 */
void Buttons_Init(void);

/**
 * @brief Один шаг опроса — вызывать из главного цикла с периодом ~BUTTONS_POLL_MS
 */
void Buttons_Poll(void);

/**
 * @brief Извлечь следующее событие из очереди
 * @param event Куда записать событие
 * @return true если событие было и извлечено; false если очередь пуста
 */
bool Buttons_PopEvent(button_event_t *event);

/**
 * @brief Сырое продебонсенное состояние кнопки прямо сейчас
 * @return true — кнопка физически удержана (после антидребезга).
 *         Не зависит от классификации short/long/chord и от флага нарушения.
 */
bool Buttons_IsHeld(button_id_t id);

/**
 * @brief Активен ли сейчас флаг нарушения правил ввода
 * @return true — было зафиксировано нарушение и ещё не все кнопки отпущены
 */
bool Buttons_IsIgnored(void);

#ifdef __cplusplus
}
#endif

#endif /* BUTTONS_H */
