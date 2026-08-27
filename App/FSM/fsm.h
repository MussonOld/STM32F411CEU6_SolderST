/**
 * @file fsm.h
 * @brief FSM главного экрана: активный канал, разбор событий Buttons в
 *        изменения Settings/State, ускоренное изменение уставки при
 *        удержании UP/DN.
 *
 * Единственный писатель в State_SetSetpointTemp/State_SetEnabled со стороны
 * пользовательского ввода (см. правило в шапке settings.h — Settings сам не
 * трогает State, это делает этот модуль). Та же пара полей синхронизируется
 * этим модулем и при старте — см. InputFSM_SyncStateFromSettings().
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
 *  - UP+DN длинный аккорд: переход в сервисное меню (SCREEN_MODE_SERVICE),
 *    см. menu.h — навигация/редактирование там же передаются в Menu.
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
    SCREEN_MODE_SERVICE /* сервисное меню, содержимое/навигация — см. menu.h */
} screen_mode_t;

/**
 * @brief Инициализация: активный канал = CHANNEL_SOLDER, режим = MAIN
 */
void InputFSM_Init(void);

/**
 * @brief Отразить в State то, что реально загружено в Settings — для КАЖДОГО
 *        канала: State_SetSetpointTemp(ch, FIXED_FROM_INT(Settings_GetTarget(ch))).
 *
 * Не пользовательский ввод (в отличие от остальных писателей этих полей в
 * этом модуле) — вызвать РОВНО ОДИН РАЗ при старте, сразу после
 * Settings_Load(), до входа в главный цикл. Без этого State_SetpointTemp
 * остаётся 0 (дефолт State_Init()) до первого нажатия SET/UP/DN, хотя экран
 * (читает Settings напрямую) и Settings уже показывают загруженное значение
 * — рассинхрон State/Settings, который заметен будущему Control/PID, если
 * тот читает State_GetSetpointTemp() до первого нажатия кнопки.
 */
void InputFSM_SyncStateFromSettings(void);

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
