/**
 * @file menu.h
 * @brief Сервисное меню: два уровня (User/Expert), навигация и редактирование
 *        параметров текущего активного канала (см. InputFSM_GetActiveChannel()).
 *
 * Владелец экрана меню (не главного) — вызывается из fsm.c, пока
 * InputFSM_GetScreenMode() == SCREEN_MODE_SERVICE. Fsm сам перехватывает
 * глобальные жесты выхода (короткие SET1/SET3, длинный аккорд UP+DN) ДО
 * передачи события сюда — Menu их никогда не видит.
 *
 * ---- Уровни ----
 * User (по умолчанию при входе в меню):
 *   Выход, Bzzz (ON/OFF, глобальный флаг — см. SETTINGS_FLAG_BUZZER_BIT),
 *   PreslipTime (Settings_GetPreSleepTimeout, канала), PreslipTemp
 *   (Settings_GetPresleepTemp, канала), Standby (Settings_GetSleepTimeout,
 *   канала), Сброс (см. ниже), Expert (переход на уровень Expert).
 *
 * Expert — войти можно только длинным SET2 на пункте "Expert": первое
 * длинное нажатие показывает предупреждение (Menu_IsShowingExpertWarning()),
 * второе длинное — открывает уровень. Короткое нажатие на "Expert" ничего
 * не делает. Пункты: Выход (обратно в User, НЕ в главный экран),
 * Kp/Ki/Kd/Slope/Bias (канала), Сброс (см. ниже, идёт последним, после Bias).
 *
 * ---- Пункт "Сброс" (есть на обоих уровнях, User и Expert) ----
 * Пункт-действие: сбрасывает к значениям по умолчанию поля ТЕКУЩЕГО уровня
 * меню для ТЕКУЩЕГО активного канала (Settings_ResetUserDefaults() /
 * Settings_ResetExpertDefaults() — точечно, EEPROM не стирается, в отличие
 * от Settings_ResetToDefaults()). На уровне User сброс также затрагивает
 * глобальный флаг Bzzz (общий на оба канала).
 *
 * Требует подтверждения: короткий SET2 на пункте "Сброс" переводит в режим
 * подтверждения (Menu_IsShowingResetConfirm()) — короткий SET2 в этом режиме
 * отменяет (возврат в список), длинный SET2 выполняет сброс и переводит в
 * режим показа результата (Menu_IsShowingResetDone()) на MENU_RESET_DONE_MS,
 * после чего меню автоматически возвращается в список (тот же уровень и
 * курсор, где был выбран "Сброс" — экран меню не покидается).
 *
 * ---- Навигация ----
 * UP/DN — циклический выбор пункта текущего уровня, когда ничего не
 * редактируется. TOOLS переключает активный канал (обрабатывается в fsm.c,
 * ДО меню — Menu каждый раз читает InputFSM_GetActiveChannel() заново, сам
 * TOOLS не видит и не обрабатывает).
 *
 * SET2 — вход/выход из редактирования выбранного пункта (кроме Выход/Сброс/
 * Expert — это пункты-действия, срабатывают сразу по короткому SET2, без
 * отдельного режима редактирования). В режиме редактирования: Bzzz — UP/DN
 * (короткое) переключает ON/OFF; числовые пункты — короткое UP/DN ±1,
 * длинное — авто-повтор шагом MENU_ACCEL_STEP (без многофазного ускорения
 * главного экрана — упрощённо, см. Menu_Poll()).
 *
 * ---- Персист ----
 * Изменения применяются в Settings сразу (клампятся там же), но физическая
 * запись в EEPROM откладывается до полного выхода из меню в главный экран
 * (см. fsm.c: вызывает Settings_Save() при возврате в SCREEN_MODE_MAIN, а
 * не полагается на обычный отложенный таймер Settings_Poll()). Переход
 * Expert -> User (через пункт "Выход" на уровне Expert) НЕ считается полным
 * выходом и запись не форсирует.
 */

#ifndef MENU_H
#define MENU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "buttons.h" /* button_event_t */

/**
 * @brief Результат обработки события меню — сигнал наверх (fsm.c), нужно
 *        ли выполнить полный выход в главный экран
 */
typedef enum {
    MENU_ACTION_NONE = 0,
    MENU_ACTION_EXIT_TO_MAIN, /**< Пункт "Выход" на уровне User — fsm.c должен переключить режим экрана и сохранить Settings */
} menu_action_t;

/**
 * @brief Сбросить меню в начальное состояние: уровень User, курсор на первом
 *        пункте, режим редактирования выключен. Вызывать при каждом входе
 *        в сервисный экран (позиция/уровень не запоминаются между визитами).
 */
void Menu_Init(void);

/**
 * @brief Разобрать одно событие кнопок. Вызывать из fsm.c вместо обычной
 *        обработки, пока экран в режиме SCREEN_MODE_SERVICE — но только
 *        после того как fsm.c проверил и обработал глобальные жесты выхода.
 */
menu_action_t Menu_HandleEvent(const button_event_t *ev);

/**
 * @brief Продвинуть авто-повтор при удержании UP/DN в режиме редактирования
 *        числового параметра. Вызывать из fsm.c каждую итерацию главного
 *        цикла, пока экран в режиме SCREEN_MODE_SERVICE (дёшево — сравнение
 *        по HAL_GetTick(), либо мгновенный выход, если не редактируем).
 */
void Menu_Poll(void);

/* ---- Для Screen (рендер) ---- */

/** @brief "Настройка Паяльник" / "Настройка Отсос" — по текущему активному каналу */
const char *Menu_GetTitle(void);

/** @brief Число пунктов в ТЕКУЩЕМ уровне (7 для User, 7 для Expert) */
uint8_t Menu_GetItemCount(void);

/** @brief Название пункта по индексу текущего уровня (например "Bzzz", "Kp") */
const char *Menu_GetItemLabel(uint8_t index);

/**
 * @brief Текстовое представление значения пункта (например "ON"/"OFF", "300")
 *        в buf. Пусто для пунктов-действий (Выход/Сброс/Expert).
 */
void Menu_GetItemValueText(uint8_t index, char *buf, uint8_t buf_size);

/** @brief Индекс выбранного сейчас пункта текущего уровня */
uint8_t Menu_GetCursor(void);

/** @brief true — значение выбранного пункта сейчас редактируется (UP/DN меняют его, а не курсор) */
bool Menu_IsEditing(void);

/** @brief true — показывается предупреждение перед первым входом в Expert (см. докстринг файла) */
bool Menu_IsShowingExpertWarning(void);

/** @brief Строка предупреждения Expert, index 0..2 (три строки текста) */
const char *Menu_GetExpertWarningLine(uint8_t line_index);

/** @brief true — показывается промт подтверждения пункта "Сброс" (см. докстринг файла) */
bool Menu_IsShowingResetConfirm(void);

/** @brief Строка промта подтверждения сброса, index 0..2 (три строки текста) */
const char *Menu_GetResetConfirmLine(uint8_t line_index);

/** @brief true — показывается сообщение о выполненном сбросе (см. докстринг файла) */
bool Menu_IsShowingResetDone(void);

/** @brief Строка сообщения о выполненном сбросе, index 0..2 (три строки текста) */
const char *Menu_GetResetDoneLine(uint8_t line_index);

#ifdef __cplusplus
}
#endif

#endif /* MENU_H */
