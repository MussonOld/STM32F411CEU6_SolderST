/**
 * @file error.h
 * @brief Интерпретация ошибок из других модулей (EEPROM, диагностика
 *        инструментов) в единый источник истины: что показывать на дисплее,
 *        что блокировать.
 *
 * Чистый интерпретатор — сам ничего не диагностирует, только принимает
 * готовые факты от других модулей (Settings_Load() → Error_ReportEepromStatus();
 * будущий модуль диагностики ADS1220/нагревателя → Error_SetRtdOpen/SetHeaterOpen)
 * и переводит их в состояние, которым пользуются Fsm (блокировка
 * фокуса/управления) и Screen (цвета/сообщения).
 *
 * Не путать с HAL-сгенерированным Error_Handler() в main.c — это разные,
 * не связанные друг с другом вещи (тот — обработчик фатальных сбоев HAL-
 * инициализации, этот модуль — доменная логика станции).
 *
 * ---- EEPROM ----
 * Settings_Load() возвращает SettingsLoadStatus_t (см. settings.h) —
 * передать РЕЗУЛЬТАТ в Error_ReportEepromStatus() сразу после вызова, один
 * раз при старте:
 *  - SETTINGS_LOAD_OK      — ничего не делает.
 *  - SETTINGS_LOAD_INVALID — данные были не наши/повреждены, дефолты уже
 *    записаны обратно (см. settings.c) — транзитное сообщение в инфозоне
 *    на ERROR_EEPROM_TRANSIENT_MS (5 сек), работу не блокирует.
 *  - SETTINGS_LOAD_IO_ERROR — микросхема не отвечает — "авария EEPROM" на
 *    весь сеанс (Error_IsEepromAlarmActive() остаётся true до перезагрузки;
 *    замена микросхемы происходит только при выключенном питании, так что
 *    самостоятельно эта авария за время сеанса не пройдёт). Работу тоже не
 *    блокирует — просто EEPROM не персистит.
 *
 * ---- Инструменты (RTD/нагреватель) ----
 * ЗАГЛУШКА: реальный источник данных (диагностика через ADS1220 —
 * обрыв/КЗ RTD, обрыв цепи нагревателя) ещё не написан — плата не
 * распаяна. Error_SetRtdOpen()/Error_SetHeaterOpen() — точка входа,
 * которую будущий модуль диагностики будет дёргать каждый цикл опроса;
 * пока их никто не вызывает, оба флага по умолчанию false (обрыва нет).
 *
 * Solder_Test/Desolder_Test: высокий уровень = исправно (см. обсуждение).
 *
 * Комбинация статусов:
 *  - RTD открыт + нагреватель открыт → TOOL_FAULT_DISCONNECTED
 *    ("инструмент не подключен"). Заголовок и текущая температура —
 *    красным. Текстового сообщения нет (см. Error_GetChannelFaultMessage()).
 *  - Только RTD открыт → TOOL_FAULT_RTD_OPEN. Заголовок и температура —
 *    красным, плюс сообщение "Обрыв RTD" (выводится Screen на месте целевой
 *    температуры — Comic_60_dig, которым рисуется текущая температура, не
 *    содержит кириллицы физически, для текста нужен шрифт AntiquaB).
 *  - Только нагреватель открыт → TOOL_FAULT_HEATER_OPEN. Аналогично,
 *    сообщение "Обрыв нагревателя".
 *  - Ни того ни другого → TOOL_FAULT_NONE, канал работает как обычно.
 *
 * Для ЛЮБОГО tool_fault_t != NONE: нагрев и управление (SET1/2/3, UP/DN)
 * заблокированы (Error_IsChannelBlocked()), фокус (TOOLS) на этот канал не
 * передаётся — обе проверки на стороне Fsm. Нагрев физически ещё не
 * реализован (Control-слой отложен до платы), Error_IsChannelBlocked()
 * заранее готов как точка интеграции для него.
 */

#ifndef ERROR_H
#define ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "channel.h"
#include "settings.h" /* SettingsLoadStatus_t */

/** @brief Сколько показывать транзитное сообщение EEPROM (SETTINGS_LOAD_INVALID) в инфозоне */
#define ERROR_EEPROM_TRANSIENT_MS (5000U)

/**
 * @brief Неисправность инструмента (RTD/нагреватель канала)
 */
typedef enum {
    TOOL_FAULT_NONE = 0,
    TOOL_FAULT_RTD_OPEN,      /**< Обрыв RTD, нагреватель цел */
    TOOL_FAULT_HEATER_OPEN,   /**< Обрыв нагревателя, RTD цел */
    TOOL_FAULT_DISCONNECTED,  /**< Обрыв и RTD, и нагревателя — инструмент не подключен */
} tool_fault_t;

void Error_Init(void);

/**
 * @brief Обслужить таймер транзитного сообщения EEPROM. Вызывать из
 *        главного цикла каждую итерацию (дёшево — сравнение по HAL_GetTick()).
 */
void Error_Poll(void);

/* ---- EEPROM ---- */

/** @brief Вызвать один раз при старте, сразу после Settings_Load() */
void Error_ReportEepromStatus(SettingsLoadStatus_t status);

/** @brief true — авария EEPROM активна (весь сеанс, до перезагрузки) */
bool Error_IsEepromAlarmActive(void);

/**
 * @brief Текст для инфозоны прямо сейчас, или NULL если показывать нечего.
 *        Авария (весь сеанс) имеет приоритет над транзитным сообщением.
 */
const char *Error_GetInfoZoneMessage(void);

/* ---- Диагностика инструментов (см. докстринг файла — сейчас заглушка) ---- */

void Error_SetRtdOpen(channel_id_t ch, bool open);
void Error_SetHeaterOpen(channel_id_t ch, bool open);

/** @brief Текущая неисправность канала (комбинация RTD/heater open, см. докстринг) */
tool_fault_t Error_GetToolFault(channel_id_t ch);

/** @brief true — нагрев и управление (SET/UP/DN) заблокированы, фокус не передаётся */
bool Error_IsChannelBlocked(channel_id_t ch);

/** @brief true — для Screen: заголовок и текущая температура канала красным */
bool Error_IsChannelFaulted(channel_id_t ch);

/**
 * @brief Текст сообщения об обрыве (заменяет собой целевую температуру на
 *        экране, см. докстринг файла) или NULL, если сообщения нет —
 *        либо неисправности нет (TOOL_FAULT_NONE), либо это
 *        TOOL_FAULT_DISCONNECTED (для него сообщения не предусмотрено,
 *        только цвет).
 */
const char *Error_GetChannelFaultMessage(channel_id_t ch);

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
