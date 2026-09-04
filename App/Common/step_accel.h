/**
 * @file step_accel.h
 * @brief Многофазный авто-повтор UP/DN при удержании кнопки (общий для
 *        главного экрана — FSM/fsm.c — и редактирования пункта меню —
 *        Menu/menu.c; раньше был продублирован в обоих один в один).
 *
 * Алгоритм: шаг 1 (MENU/FSM)_ACCEL_STEP1_ITERATIONS раз -> округление до
 * ближайших 5 -> шаг 5 STEP5_ITERATIONS раз -> округление до ближайших 10 ->
 * шаг 10 без ограничения по числу итераций, до отпускания кнопки.
 *
 * Модуль не хранит редактируемое значение сам — читает/пишет его через
 * колбэки get/set, которые предоставляет вызывающий модуль (там же, где
 * раньше лежала копия этой логики). Клампинг диапазона — забота set().
 */

#ifndef STEP_ACCEL_H
#define STEP_ACCEL_H

#include <stdint.h>
#include <stdbool.h>
#include "buttons.h" /* button_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Интервал между шагами при удержании — ВРЕМЕННЫЙ, не уточнялся, подобрать
 * по ощущениям на реальном железе. */
#define STEP_ACCEL_REPEAT_MS        (150U)
#define STEP_ACCEL_STEP1_ITERATIONS (10U)
#define STEP_ACCEL_STEP5_ITERATIONS (5U)
#define STEP_ACCEL_STEP_1           (1U)
#define STEP_ACCEL_STEP_5           (5U)
#define STEP_ACCEL_STEP_10          (10U)

typedef enum {
    STEP_ACCEL_PHASE_1 = 0,
    STEP_ACCEL_PHASE_5,
    STEP_ACCEL_PHASE_10,
} step_accel_phase_t;

/** @brief Прочитать текущее значение (уже закламленное предыдущим set) */
typedef uint16_t (*step_accel_get_fn)(void *ctx);
/** @brief Записать новое значение; клампинг по диапазону — на стороне set() */
typedef void     (*step_accel_set_fn)(void *ctx, uint16_t value);

typedef struct {
    bool                active;
    button_id_t         button;
    step_accel_phase_t  phase;
    uint8_t             iteration;
    uint32_t            last_tick;
    step_accel_get_fn   get;
    step_accel_set_fn   set;
    void               *ctx;
} step_accel_t;

/**
 * @brief Запустить авто-повтор: сразу делает первый шаг (не дожидаясь
 *        интервала STEP_ACCEL_REPEAT_MS).
 */
void StepAccel_Start(step_accel_t *sa, button_id_t btn,
                      step_accel_get_fn get, step_accel_set_fn set, void *ctx,
                      uint32_t now);

/** @brief Остановить, сбросить фазу — без применения шага. */
void StepAccel_Stop(step_accel_t *sa);

/**
 * @brief Вызывать из Poll, пока условия удержания истинны (кнопка ещё
 *        нажата, доп. блокировки и т.п. — проверка на стороне вызывающего;
 *        если условие ложно, вызывающий обязан сам позвать StepAccel_Stop()
 *        и не звать Tick()). Сама проверяет интервал и не делает лишней
 *        работы, если он ещё не истёк.
 */
void StepAccel_Tick(step_accel_t *sa, uint32_t now);

/**
 * @brief Разовый шаг без фазового ускорения (короткое нажатие UP/DN —
 *        всегда шаг 1, минуя фазовую машину).
 */
void StepAccel_ApplyDelta(step_accel_get_fn get, step_accel_set_fn set, void *ctx,
                           int32_t sign, uint16_t step);

#ifdef __cplusplus
}
#endif

#endif /* STEP_ACCEL_H */
