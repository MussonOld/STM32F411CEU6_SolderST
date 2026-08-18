/**
 * @file state.h
 * @brief Модель состояния канала паяльной станции — общая точка данных
 *        между Control (PID) и UI слоями. Ни один из них не знает про другой.
 *
 * Control пишет current_temp/heater_active, читает setpoint_temp.
 * UI только читает — ничего не пишет.
 * Ввод (кнопки) пишет setpoint_temp/enabled.
 *
 * Синхронизация — через volatile для отдельных полей; для 2-канальной
 * станции этого достаточно (нет строгих консистентность-требований между
 * полями одной структуры при чтении из UI).
 */

#ifndef STATE_H
#define STATE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Идентификатор канала станции
 */
typedef enum {
    CHANNEL_SOLDER = 0,
    CHANNEL_DESOLDER,
    CHANNEL_COUNT
} channel_id_t;

/**
 * @brief Инициализировать состояние всех каналов (обнулить)
 */
void State_Init(void);

/* ---- Температура ---- */

/** @brief Текущая температура канала, °C. Пишет Control. */
void  State_SetCurrentTemp(channel_id_t ch, float temp_c);
float State_GetCurrentTemp(channel_id_t ch);

/** @brief Уставка канала, °C. Пишет ввод (кнопки). */
void  State_SetSetpointTemp(channel_id_t ch, float temp_c);
float State_GetSetpointTemp(channel_id_t ch);

/* ---- Нагреватель ---- */

/** @brief Состояние выхода нагревателя (Solder_On/Desolder_On). Пишет Control. */
void State_SetHeaterActive(channel_id_t ch, bool active);
bool State_IsHeaterActive(channel_id_t ch);

/* ---- Датчик "жало в подставке" ---- */

/** @brief Solder_Test/Desolder_Test. Пишет опрос входов (Control или отдельный Input-слой). */
void State_SetInStand(channel_id_t ch, bool in_stand);
bool State_IsInStand(channel_id_t ch);

/* ---- Включён ли канал пользователем ---- */

void State_SetEnabled(channel_id_t ch, bool enabled);
bool State_IsEnabled(channel_id_t ch);

#ifdef __cplusplus
}
#endif

#endif /* STATE_H */