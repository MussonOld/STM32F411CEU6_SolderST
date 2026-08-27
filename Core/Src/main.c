/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "dma.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "display.h"
#include "gfx.h"
#include "fonts.h"
#include "text_field.h"
#include "screen.h"
#include "buttons.h"
#include "fsm.h"
#include "settings.h"
#include "state.h"
#include "error.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM5_Init();
  MX_SPI2_Init();
  MX_TIM10_Init();
  /* USER CODE BEGIN 2 */
  Display_Init();
  Display_SetWindow(0, 0, 319, 239);
  Display_FillColorDMA(DISPLAY_RGB565(0, 0, 0), 320 * 240);
  while (Display_IsBusy()) { } /* Однократное ожидание при старте, до входа в главный цикл */

  TextField_Init();

  State_Init();
  Settings_Init();   /* дефолты в RAM на случай сбоя чтения ниже */
  Error_Init();
  Error_ReportEepromStatus(Settings_Load()); /* поверх дефолтов — то, что реально сохранено в EEPROM (или ошибка/стёртый чип, см. settings.c); статус — в Error, для сообщения в инфозоне */
  InputFSM_SyncStateFromSettings(); /* без этого State.setpoint_temp==0 до первого нажатия SET/UP/DN — см. fsm.h */

  Buttons_Init();
  InputFSM_Init();

  Screen_Init();     /* статика (разделитель) + геометрия строк, после TextField_Init() */
  Screen_Update();   /* первое наполнение содержимым (только помечает строки грязными —
                       * реальная отрисовка стартует в первых итерациях главного цикла,
                       * через TextField_Process()) */


  /* Тайминг-гейт для модулей, чей дебаунс/таймеры считаются В ОПРОСАХ, а не
   * во времени (Buttons_Poll(), позже сюда же встанет Sleep_Poll()) — оба
   * задокументированы как "вызывать с периодом ~10 мс", но без этого гейта
   * они опрашивались бы на каждой итерации главного цикла (единицы мкс),
   * и дебаунс в 3 стабильных опроса превращался бы в единицы мкс вместо
   * заявленных 30 мс. Без дрейфа (см. также счётчик в предыдущей версии
   * main.c) — += period, а не = HAL_GetTick(). */
  uint32_t poll10ms_last_tick = HAL_GetTick();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (HAL_GetTick() - poll10ms_last_tick >= BUTTONS_POLL_MS) {
        poll10ms_last_tick += BUTTONS_POLL_MS;
        Buttons_Poll();
        /* Sleep_Poll(); — подключить сюда же, когда модуль сна будет
         * интегрирован в main.c: его дебаунс/таймеры считаются тем же
         * способом (см. App/Sleep/sleep.h), тот же гейт ему и нужен. */
    }

    InputFSM_Poll();
    Settings_Poll();  /* отложенная запись в EEPROM — сама решает, когда физически писать */
    Error_Poll();     /* таймер транзитного сообщения EEPROM в инфозоне */

    Screen_Update();  /* обновить содержимое строк из State/Settings/InputFSM */
    TextField_Process(); /* рендер — сам по себе, догоняет данные по мере готовности DMA */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 192;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
