/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PWR_ON_Pin GPIO_PIN_13
#define PWR_ON_GPIO_Port GPIOC
#define SPI1_CS1_Pin GPIO_PIN_14
#define SPI1_CS1_GPIO_Port GPIOC
#define Disp_DC_Pin GPIO_PIN_3
#define Disp_DC_GPIO_Port GPIOA
#define Disp_RST_Pin GPIO_PIN_4
#define Disp_RST_GPIO_Port GPIOA
#define Dock_Pin GPIO_PIN_0
#define Dock_GPIO_Port GPIOB
#define Dock_EXTI_IRQn EXTI0_IRQn
#define Desolder_Test_Pin GPIO_PIN_1
#define Desolder_Test_GPIO_Port GPIOB
#define Solder_Test_Pin GPIO_PIN_2
#define Solder_Test_GPIO_Port GPIOB
#define Btn_Pump_Pin GPIO_PIN_12
#define Btn_Pump_GPIO_Port GPIOB
#define Pump_On_Pin GPIO_PIN_13
#define Pump_On_GPIO_Port GPIOB
#define SET1_Pin GPIO_PIN_8
#define SET1_GPIO_Port GPIOA
#define SET2_Pin GPIO_PIN_9
#define SET2_GPIO_Port GPIOA
#define SET3_Pin GPIO_PIN_10
#define SET3_GPIO_Port GPIOA
#define DN_Pin GPIO_PIN_11
#define DN_GPIO_Port GPIOA
#define UP_Pin GPIO_PIN_12
#define UP_GPIO_Port GPIOA
#define TOOLS_Pin GPIO_PIN_15
#define TOOLS_GPIO_Port GPIOA
#define BEEP_Pin GPIO_PIN_3
#define BEEP_GPIO_Port GPIOB
#define Solder_On_Pin GPIO_PIN_4
#define Solder_On_GPIO_Port GPIOB
#define Desolder_On_Pin GPIO_PIN_5
#define Desolder_On_GPIO_Port GPIOB
#define ADS1220_Solder_CS_Pin GPIO_PIN_8
#define ADS1220_Solder_CS_GPIO_Port GPIOB
#define ADS1220_Desolder_CS_Pin GPIO_PIN_9
#define ADS1220_Desolder_CS_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
