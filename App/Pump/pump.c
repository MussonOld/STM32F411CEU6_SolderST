/**
 * @file pump.c
 * @brief Реализация pump.h — правила и обоснования в шапке заголовка.
 */

#include "pump.h"
#include "channel.h"
#include "state.h"
#include "error.h"
#include "main.h"          /* Pump_On_*, Btn_Pump_* */
#include "stm32f4xx_hal.h"

static bool s_on;

static void pump_write(bool on)
{
    /* Pump_On активный низкий. */
    HAL_GPIO_WritePin(Pump_On_GPIO_Port, Pump_On_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
    s_on = on;
}

void Pump_Init(void)
{
    pump_write(false);
}

void Pump_Poll(void)
{
    /* Btn_Pump активная низкая: нажата = RESET. */
    bool pressed = (HAL_GPIO_ReadPin(Btn_Pump_GPIO_Port, Btn_Pump_Pin) == GPIO_PIN_RESET);
    bool allowed = State_IsEnabled(CHANNEL_DESOLDER)
                && !Error_IsChannelBlocked(CHANNEL_DESOLDER);

    pump_write(pressed && allowed);
}

bool Pump_IsOn(void)
{
    return s_on;
}
