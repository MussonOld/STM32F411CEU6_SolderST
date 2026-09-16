/**
 * @file error.c
 * @brief Реализация error.h — см. правила в шапке заголовка.
 */

#include "error.h"
#include "stm32f4xx_hal.h" /* HAL_GetTick() — таймер транзитного сообщения EEPROM */

typedef struct {
    bool        adc_fault;
    rtd_state_t rtd;
    bool        heater_open;
} tool_diag_t;

static tool_diag_t s_tool[CHANNEL_COUNT];

static bool         s_eeprom_alarm;             /* авария EEPROM — весь сеанс, не сбрасывается */
static bool         s_eeprom_transient_active;
static uint32_t     s_eeprom_transient_start;
static const char  *s_eeprom_transient_msg;

static bool         s_psu_fault;                /* true = БП неисправен (Pok высокий) — НЕ защёлкивается, снимается само по возврату Pok в норму, см. Error_ReportPsuStatus() */

void Error_Init(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_tool[ch].adc_fault = false;
        s_tool[ch].rtd = RTD_STATE_OK;
        s_tool[ch].heater_open = false;
    }
    s_eeprom_alarm = false;
    s_eeprom_transient_active = false;
    s_eeprom_transient_start = 0;
    s_eeprom_transient_msg = NULL;
    s_psu_fault = true; /* фейл-сейф до первого реального Error_ReportPsuStatus() из главного цикла (см. main.c) — считаем БП неисправным, пока не подтверждено обратное */
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
    if (s_psu_fault) {
        return "БП не исправен"; /* высший приоритет — даже над аварией EEPROM: без исправного питания EEPROM всё равно не имеет значения */
    }
    if (s_eeprom_alarm) {
        return "АВАРИЯ EEPROM"; /* приоритет выше транзитного сообщения */
    }
    if (s_eeprom_transient_active) {
        return s_eeprom_transient_msg;
    }
    return NULL;
}

void Error_ReportPsuStatus(bool ok)
{
    s_psu_fault = !ok;
}

bool Error_IsPsuFaultActive(void)
{
    return s_psu_fault;
}

void Error_Poll(void)
{
    if (s_eeprom_transient_active &&
        (HAL_GetTick() - s_eeprom_transient_start) >= ERROR_EEPROM_TRANSIENT_MS) {
        s_eeprom_transient_active = false;
    }
}

void Error_SetAdcFault(channel_id_t ch, bool fault)
{
    if (!channel_valid(ch)) return;
    s_tool[ch].adc_fault = fault;
}

void Error_SetRtdState(channel_id_t ch, rtd_state_t state)
{
    if (!channel_valid(ch)) return;
    s_tool[ch].rtd = state;
}

void Error_SetHeaterOpen(channel_id_t ch, bool open)
{
    if (!channel_valid(ch)) return;
    s_tool[ch].heater_open = open;
}

tool_fault_t Error_GetToolFault(channel_id_t ch)
{
    if (!channel_valid(ch)) return TOOL_FAULT_NONE;

    /* Неисправность АЦП — высший приоритет, см. докстринг error.h: если
     * сам АЦП не отвечает, RTD/нагреватель по его показаниям недостоверны. */
    if (s_tool[ch].adc_fault) return TOOL_FAULT_ADC_FAULT;

    rtd_state_t rtd = s_tool[ch].rtd;
    bool heater_bad = s_tool[ch].heater_open;

    /* КЗ RTD имеет приоритет над состоянием нагревателя — по таблице оно
     * даёт TOOL_FAULT_RTD_SHORT независимо от того, цел нагреватель или
     * нет (комбинация "неисправный нагреватель + КЗ RTD" редкая, но
     * возможна; см. докстринг error.h). */
    if (rtd == RTD_STATE_SHORT) return TOOL_FAULT_RTD_SHORT;

    if (rtd == RTD_STATE_OPEN) {
        return heater_bad ? TOOL_FAULT_DISCONNECTED : TOOL_FAULT_RTD_OPEN;
    }
    return heater_bad ? TOOL_FAULT_HEATER_OPEN : TOOL_FAULT_NONE;
}

bool Error_IsChannelBlocked(channel_id_t ch)
{
    if (s_psu_fault) return true; /* глобально, оба канала — БП неисправен, нагрев/диагностику не имеет смысла делать без исправного питания */
    return Error_GetToolFault(ch) != TOOL_FAULT_NONE;
}

bool Error_IsChannelFaulted(channel_id_t ch)
{
    /* Только аварии — "инструмент не подключен" красным НЕ красим (см.
     * докстринг error.h): это штатное состояние, а не отказ. */
    return Error_IsChannelAlarm(ch);
}

bool Error_IsChannelIdle(channel_id_t ch)
{
    return Error_GetToolFault(ch) == TOOL_FAULT_DISCONNECTED;
}

bool Error_IsChannelAlarm(channel_id_t ch)
{
    tool_fault_t f = Error_GetToolFault(ch);
    return (f != TOOL_FAULT_NONE) && (f != TOOL_FAULT_DISCONNECTED);
}

const char *Error_GetChannelFaultMessage(channel_id_t ch)
{
    switch (Error_GetToolFault(ch)) {
        case TOOL_FAULT_ADC_FAULT:   return "Авария АЦП";
        case TOOL_FAULT_RTD_SHORT:   return "КЗ RTD";
        case TOOL_FAULT_RTD_OPEN:    return "Обрыв RTD";
        case TOOL_FAULT_HEATER_OPEN: return "Обрыв нагревателя";
        case TOOL_FAULT_DISCONNECTED:
        case TOOL_FAULT_NONE:
        default:
            return NULL;
    }
}
