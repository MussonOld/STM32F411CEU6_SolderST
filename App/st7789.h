/**
 * @file st7789.h
 * @brief Приватные константы контроллера ST7789 (команды, MADCTL, тайминги)
 *
 * Не инклюдится ничем, кроме st7789.c. Верхний слой работает только через display.h.
 */

#ifndef ST7789_H
#define ST7789_H

/* Физическое разрешение RAM контроллера (портретная ориентация "как есть") */
#define ST7789_RAM_WIDTH   240
#define ST7789_RAM_HEIGHT  320

/* Команды (Datasheet ST7789, раздел "Command list") */
#define ST7789_CMD_SWRESET  0x01
#define ST7789_CMD_SLPIN    0x10
#define ST7789_CMD_SLPOUT   0x11
#define ST7789_CMD_INVOFF   0x20
#define ST7789_CMD_INVON    0x21
#define ST7789_CMD_DISPOFF  0x28
#define ST7789_CMD_DISPON   0x29
#define ST7789_CMD_CASET    0x2A
#define ST7789_CMD_RASET    0x2B
#define ST7789_CMD_RAMWR    0x2C
#define ST7789_CMD_MADCTL   0x36
#define ST7789_CMD_COLMOD   0x3A

/* MADCTL биты (Memory Access Control) */
#define ST7789_MADCTL_MY    0x80  /* Row address order   */
#define ST7789_MADCTL_MX    0x40  /* Column address order */
#define ST7789_MADCTL_MV    0x20  /* Row/Column exchange  */
#define ST7789_MADCTL_RGB   0x00  /* Порядок цвета RGB (не BGR) */

/* COLMOD: 0x55 = 16 бит/пиксель (RGB565) */
#define ST7789_COLMOD_16BPP 0x55

/* Тайминги (datasheet, мс) */
#define ST7789_DELAY_RESET_MS    10   /* Длительность низкого уровня RESX   */
#define ST7789_DELAY_AFTER_RST_MS 120 /* Пауза после аппаратного reset      */
#define ST7789_DELAY_SLPOUT_MS   120  /* Пауза после Sleep Out              */

#endif /* ST7789_H */