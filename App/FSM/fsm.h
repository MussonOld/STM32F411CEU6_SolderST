/**
 * @file fsm.h
 * @brief FSM главного экрана: активный канал, разбор событий Buttons в
 *        изменения Settings/State, ускоренное изменение уставки при
 *        удержании UP/DN.
 *
 * Единственный писатель в State_SetSetpointTemp/State_SetEnabled со стороны
 * пользовательского ввода (см. правило в шапке settings.h — Settings сам не
 * трогает State, это делает этот модуль).
 *
 * InputFSM_Poll() — вызывать из главного цикла каждую итерацию, ПОСЛЕ
 * Buttons_Poll(). Не блокирует.
 *
 * Правила (зафиксированы в обсуждении):
 *  - TOOLS (короткое, единственное разрешённое для неё) переключает активный
 *    канал. При старте активен CHANNEL_SOLDER.
 *  - Все остальные кнопки управляют ТОЛЬКО активным каналом.
 *  - SET1/2/3 короткое: target = соответствующий preset.
 *  - SET1/2/3 длинное: соответствующий preset = текущая температура.
 *  - UP/DN короткое: target += /-= 1.
 *  - UP/DN длинное: авто-повтор с ускорением — шаг 1 десять итераций, затем
 *    округление ВВЕРХ до 5 и шаг 5 пять итераций, затем округление ВВЕРХ до
 *    10 и шаг 10 до отпускания. Основан на Buttons_IsHeld() (уровень), не на повторных
 *    событиях — Buttons отдаёт только одно LONG_PRESS на факт удержания.
 *  - UP+DN короткий аккорд: выключить активный канал (State_SetEnabled(ch,false)).
 *  - UP+DN длинный аккорд: переход в сервисное меню — пока ЗАГЛУШКА, само
 *    меню не специфицировано.
 *  - Нарушения ввода (BUTTON_EVENT_VIOLATION) пока игнорируются молча —
 *    TODO: индикация на дисплее позже.
 */

#ifndef FSM_H
#define FSM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "channel.h" /* channel_id_t */

/**
 * @brief Режим экрана
 */
typedef enum {
    SCREEN_MODE_MAIN = 0,
    SCREEN_MODE_SERVICE /* заглушка — содержимое сервисного меню не специфицировано */
} screen_mode_t;

/**
 * @brief Инициализация: активный канал = CHANNEL_SOLDER, режим = MAIN
 */
void InputFSM_Init(void);

/**
 * @brief Один шаг: разобрать очередь событий Buttons + продвинуть авто-повтор UP/DN
 *
 * Звать из главного цикла каждую итерацию, после Buttons_Poll().
 */
void InputFSM_Poll(void);

/** @brief Текущий активный канал (для Display — какую половину рисовать ярко) */
channel_id_t InputFSM_GetActiveChannel(void);

/** @brief Текущий режим экрана (для Display — что рисовать) */
screen_mode_t InputFSM_GetScreenMode(void);

#ifdef __cplusplus
}
#endif

#endif /* FSM_H */
