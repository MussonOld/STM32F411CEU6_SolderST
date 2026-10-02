/**
 * Заглушка HAL для хост-тестов (не входит в сборку прошивки).
 * Даёт ровно то, что нужно buttons.c, sleep.c и Core/Inc/main.h: номера пинов,
 * GPIOx->IDR, HAL_GetTick() и HAL_GPIO_ReadPin(). Значения задаёт тест.
 */
#ifndef STUB_STM32F4XX_HAL_H
#define STUB_STM32F4XX_HAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define GPIO_PIN_0 ((uint16_t)0x0001U)
#define GPIO_PIN_1 ((uint16_t)0x0002U)
#define GPIO_PIN_2 ((uint16_t)0x0004U)
#define GPIO_PIN_3 ((uint16_t)0x0008U)
#define GPIO_PIN_4 ((uint16_t)0x0010U)
#define GPIO_PIN_5 ((uint16_t)0x0020U)
#define GPIO_PIN_6 ((uint16_t)0x0040U)
#define GPIO_PIN_7 ((uint16_t)0x0080U)
#define GPIO_PIN_8 ((uint16_t)0x0100U)
#define GPIO_PIN_9 ((uint16_t)0x0200U)
#define GPIO_PIN_10 ((uint16_t)0x0400U)
#define GPIO_PIN_11 ((uint16_t)0x0800U)
#define GPIO_PIN_12 ((uint16_t)0x1000U)
#define GPIO_PIN_13 ((uint16_t)0x2000U)
#define GPIO_PIN_14 ((uint16_t)0x4000U)
#define GPIO_PIN_15 ((uint16_t)0x8000U)

typedef struct {
    volatile uint32_t IDR;
} GPIO_TypeDef;

extern GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc;
#define GPIOA (&test_gpioa)
#define GPIOB (&test_gpiob)
#define GPIOC (&test_gpioc)

uint32_t HAL_GetTick(void);

/* Для sleep.c (tests/sleep_golden): чтение уровня пина — реализацию даёт тест. */
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET = 1 } GPIO_PinState;
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin);

#endif /* STUB_STM32F4XX_HAL_H */
