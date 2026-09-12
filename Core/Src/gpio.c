/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(nPS_ON_GPIO_Port, nPS_ON_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, Disp_DC_Pin|Disp_RST_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, Pump_On_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, BEEP_Pin, GPIO_PIN_SET); /* ПРЕДПОЛОЖЕНИЕ: зуммер активный низкий (орал постоянно на
                                                       * дефолтном LOW, см. чат) — если после прошивки молчит и
                                                       * дальше НИКОГДА не пищит (никакого кода, который бы им
                                                       * управлял, в прошивке всё ещё нет — только этот дефолт),
                                                       * то полярность угадана верно; если наоборот запищал —
                                                       * значит активный высокий, вернуть на GPIO_PIN_RESET */

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, Solder_On_Pin|Desolder_On_Pin|ADS1220_Solder_CS_Pin|ADS1220_Desolder_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin : nPS_ON_Pin */
  GPIO_InitStruct.Pin = nPS_ON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(nPS_ON_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : DRDY_Solder_Pin DRDY_Desolder_Pin */
  GPIO_InitStruct.Pin = DRDY_Solder_Pin|DRDY_Desolder_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : Dock_Pin */
  GPIO_InitStruct.Pin = Dock_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : Disp_DC_Pin Disp_RST_Pin */
  GPIO_InitStruct.Pin = Disp_DC_Pin|Disp_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : Solder_Test_Pin Desolder_Test_Pin Btn_Pump_Pin */
  GPIO_InitStruct.Pin = Solder_Test_Pin|Desolder_Test_Pin|Btn_Pump_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : Pok_Pin */
  GPIO_InitStruct.Pin = Pok_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP; /* Power OK от блока питания, активный низкий (см. чат) —
                                        * подтяжка на случай открытого стока на стороне БП; если
                                        * там push-pull, лишней подтяжка не мешает */
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : Pump_On_Pin BEEP_Pin Solder_On_Pin Desolder_On_Pin
                           ADS1220_Solder_CS_Pin ADS1220_Desolder_CS_Pin */
  GPIO_InitStruct.Pin = Pump_On_Pin|BEEP_Pin|Solder_On_Pin|Desolder_On_Pin
                          |ADS1220_Solder_CS_Pin|ADS1220_Desolder_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pins : SET1_Pin SET2_Pin SET3_Pin DN_Pin
                           UP_Pin TOOLS_Pin */
  GPIO_InitStruct.Pin = SET1_Pin|SET2_Pin|SET3_Pin|DN_Pin
                          |UP_Pin|TOOLS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
