/**
 * @file error.c
 * @brief Реализация error.h — см. правила в шапке заголовка.
 */

#include "error.h"
#include "stm32f4xx_hal.h" /* HAL_GetTick() — таймер транзитного сообщения EEPROM */

typedef struct {
    bool rtd_open;
    bool heater_open;
} tool_diag_t;

static tool_diag_t s_tool[CHANNEL_COUNT];

static bool         s_eeprom_alarm;             /* авария EEPROM — весь сеанс, не сбрасывается */
static bool         s_eeprom_transient_active;
static uint32_t     s_eeprom_transient_start;
static const char  *s_eeprom_transient_msg;

void Error_Init(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_tool[ch].rtd_open = false;
        s_tool[ch].heater_open = false;
    }
    s_eeprom_alarm = false;
    s_eeprom_transient_active = false;
    s_eeprom_transient_start = 0;
    s_eeprom_transient_msg = NULL;
}

void Error_ReportEepromStatus(SettingsLoadStatus_t status)
{
    switch (status) {
        case SETTINGS_LOAD_OK:
            break;

        case SETTINGS_LOAD_INVALID:
            s_eeprom_transient_msg = "EEPROM: данные сброшены на заводские";
            s_eeprom_transient_active = true;
            s_eeprom_transient_start = HAL_GetTick();
            break;

        case SETTINGS_LOAD_IO_ERROR:
            /* Весь сеанс, до перезагрузки — замена микросхемы физически
             * возможна только при выключенном питании, так что сама по
             * себе эта авария за время работы не пройдёт. */
            s_eeprom_alarm = true;
            break;
    }
}

bool Error_IsEepromAlarmActive(void)
{
    return s_eeprom_alarm;
}

const char *Error_GetInfoZoneMessage(void)
{
    if (s_eeprom_alarm) {
        return "АВАРИЯ EEPROM"; /* приоритет выше транзитного сообщения */
    }
    if (s_eeprom_transient_active) {
        return s_eeprom_transient_msg;
    }
    return NULL;
}

void Error_Poll(void)
{
    if (s_eeprom_transient_active &&
        (HAL_GetTick() - s_eeprom_transient_start) >= ERROR_EEPROM_TRANSIENT_MS) {
        s_eeprom_transient_active = false;
    }
}

void Error_SetRtdOpen(channel_id_t ch, bool open)
{
    if (!channel_valid(ch)) return;
    s_tool[ch].rtd_open = open;
}

void Error_SetHeaterOpen(channel_id_t ch, bool open)
{
    if (!channel_valid(ch)) return;
    s_tool[ch].heater_open = open;
}

tool_fault_t Error_GetToolFault(channel_id_t ch)
{
    if (!channel_valid(ch)) return TOOL_FAULT_NONE;

    bool rtd = s_tool[ch].rtd_open;
    bool heater = s_tool[ch].heater_open;

    if (rtd && heater)  return TOOL_FAULT_DISCONNECTED;
    if (rtd)             return TOOL_FAULT_RTD_OPEN;
    if (heater)          return TOOL_FAULT_HEATER_OPEN;
    return TOOL_FAULT_NONE;
}

bool Error_IsChannelBlocked(channel_id_t ch)
{
    return Error_GetToolFault(ch) != TOOL_FAULT_NONE;
}

bool Error_IsChannelFaulted(channel_id_t ch)
{
    return Error_GetToolFault(ch) != TOOL_FAULT_NONE;
}

const char *Error_GetChannelFaultMessage(channel_id_t ch)
{
    switch (Error_GetToolFault(ch)) {
        case TOOL_FAULT_RTD_OPEN:    return "Обрыв RTD";
        case TOOL_FAULT_HEATER_OPEN: return "Обрыв нагревателя";
        case TOOL_FAULT_DISCONNECTED:
        case TOOL_FAULT_NONE:
        default:
            return NULL;
    }
}
