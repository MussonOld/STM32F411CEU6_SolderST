/**
 * @file state.c
 * @brief Реализация модели состояния каналов (см. state.h)
 */

#include "state.h"

/**
 * @brief Состояние одного канала. Поля volatile — читаются/пишутся
 *        из разных контекстов (основной цикл, прерывание таймера PID).
 */
typedef struct {
    volatile float current_temp;
    volatile float setpoint_temp;
    volatile bool  heater_active;
    volatile bool  in_stand;
    volatile bool  enabled;
} channel_state_t;

static channel_state_t s_channels[CHANNEL_COUNT];

static inline bool channel_valid(channel_id_t ch)
{
    return (ch >= 0) && (ch < CHANNEL_COUNT);
}

void State_Init(void)
{
    for (int i = 0; i < CHANNEL_COUNT; i++) {
        s_channels[i].current_temp  = 0.0f;
        s_channels[i].setpoint_temp = 0.0f;
        s_channels[i].heater_active = false;
        s_channels[i].in_stand      = true;  /* По умолчанию считаем жало в подставке */
        s_channels[i].enabled       = false;
    }
}

void State_SetCurrentTemp(channel_id_t ch, float temp_c)
{
    if (!channel_valid(ch)) return;
    s_channels[ch].current_temp = temp_c;
}

float State_GetCurrentTemp(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0.0f;
    return s_channels[ch].current_temp;
}

void State_SetSetpointTemp(channel_id_t ch, float temp_c)
{
    if (!channel_valid(ch)) return;
    s_channels[ch].setpoint_temp = temp_c;
}

float State_GetSetpointTemp(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0.0f;
    return s_channels[ch].setpoint_temp;
}

void State_SetHeaterActive(channel_id_t ch, bool active)
{
    if (!channel_valid(ch)) return;
    s_channels[ch].heater_active = active;
}

bool State_IsHeaterActive(channel_id_t ch)
{
    if (!channel_valid(ch)) return false;
    return s_channels[ch].heater_active;
}

void State_SetInStand(channel_id_t ch, bool in_stand)
{
    if (!channel_valid(ch)) return;
    s_channels[ch].in_stand = in_stand;
}

bool State_IsInStand(channel_id_t ch)
{
    if (!channel_valid(ch)) return true;
    return s_channels[ch].in_stand;
}

void State_SetEnabled(channel_id_t ch, bool enabled)
{
    if (!channel_valid(ch)) return;
    s_channels[ch].enabled = enabled;
}

bool State_IsEnabled(channel_id_t ch)
{
    if (!channel_valid(ch)) return false;
    return s_channels[ch].enabled;
}