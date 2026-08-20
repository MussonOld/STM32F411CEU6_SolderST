/**
 * @file st7789.c
 * @brief Реализация display.h для контроллера ST7789
 *
 * SPI1 (TX-only + DMA2_Stream2), CS — аппаратно на GND (не управляется программно).
 * DC = PA3 (Disp_DC), RST = PA4 (Disp_RST).
 */

#include "display.h"
#include "st7789.h"
#include "stm32f4xx_hal.h"
#include "main.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* Максимум пикселей за один DMA-транш: HAL_SPI_Transmit_DMA принимает Size
 * как uint16_t (макс. 65535 байт = 32767 пикселей по 2 байта). Берём с запасом
 * ниже границы для предсказуемости. */
#define DISPLAY_MAX_DMA_PIXELS  16384

/* ---- Внутреннее состояние драйвера ---- */
static volatile bool s_busy = false;
static Display_TxCpltCallback_t s_tx_cplt_cb = NULL;
static Display_Rotation_t s_rotation = DISPLAY_ROTATION_0;
static uint16_t s_width  = 320;
static uint16_t s_height = 240;

/* Буфер одной строки для заливки цветом без выделения полного кадра в SRAM */
#define FILL_LINE_PIXELS  320
static display_color_t s_fill_line[FILL_LINE_PIXELS];
static uint32_t s_fill_remaining = 0;
static display_color_t s_fill_color = 0;

/* Продолжение чанкованной передачи внешнего буфера пикселей (WritePixelsDMA) */
static const display_color_t *s_write_ptr = NULL;
static uint32_t s_write_remaining = 0;

/* ---- Низкоуровневые примитивы ---- */

static inline void dc_command(void) { HAL_GPIO_WritePin(Disp_DC_GPIO_Port, Disp_DC_Pin, GPIO_PIN_RESET); }
static inline void dc_data(void)    { HAL_GPIO_WritePin(Disp_DC_GPIO_Port, Disp_DC_Pin, GPIO_PIN_SET); }

static void write_command(uint8_t cmd)
{
    dc_command();
    HAL_SPI_Transmit(&hspi1, &cmd, 1, HAL_MAX_DELAY);
}

static void write_data(const uint8_t *data, uint16_t len)
{
    dc_data();
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, HAL_MAX_DELAY);
}

static void write_data_u8(uint8_t value)
{
    write_data(&value, 1);
}

/* ---- Обработчик завершения DMA (вызывается из HAL_SPI_TxCpltCallback) ---- */

void ST7789_OnDmaTxComplete(void)
{
    if (s_fill_remaining > 0) {
        uint32_t chunk = (s_fill_remaining > FILL_LINE_PIXELS) ? FILL_LINE_PIXELS : s_fill_remaining;
        s_fill_remaining -= chunk;
        dc_data();
        HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)s_fill_line, chunk * sizeof(display_color_t));
        return;
    }

    if (s_write_remaining > 0) {
        uint32_t chunk = (s_write_remaining > DISPLAY_MAX_DMA_PIXELS) ? DISPLAY_MAX_DMA_PIXELS : s_write_remaining;
        s_write_remaining -= chunk;
        dc_data();
        HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)s_write_ptr, chunk * sizeof(display_color_t));
        s_write_ptr += chunk;
        return;
    }

    s_busy = false;
    if (s_tx_cplt_cb) {
        s_tx_cplt_cb();
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI1) {
        ST7789_OnDmaTxComplete();
    }
}

/* ---- Реализация контракта display.h ---- */

Display_Status_t Display_Init(void)
{
    HAL_GPIO_WritePin(Disp_RST_GPIO_Port, Disp_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(ST7789_DELAY_RESET_MS);
    HAL_GPIO_WritePin(Disp_RST_GPIO_Port, Disp_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(ST7789_DELAY_AFTER_RST_MS);

    write_command(ST7789_CMD_SLPOUT);
    HAL_Delay(ST7789_DELAY_SLPOUT_MS);

    write_command(ST7789_CMD_COLMOD);
    write_data_u8(ST7789_COLMOD_16BPP);

    Display_SetRotation(DISPLAY_ROTATION_0);

    write_command(ST7789_CMD_INVON);
    write_command(ST7789_CMD_NORON);
    write_command(ST7789_CMD_DISPON);

    s_busy = false;
    s_fill_remaining = 0;
    s_write_remaining = 0;

    return DISPLAY_OK;
}

bool Display_IsBusy(void)
{
    return s_busy;
}

void Display_RegisterTxCpltCallback(Display_TxCpltCallback_t callback)
{
    s_tx_cplt_cb = callback;
}

Display_Status_t Display_SetRotation(Display_Rotation_t rotation)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }

    uint8_t madctl = ST7789_MADCTL_RGB;

    switch (rotation) {
        case DISPLAY_ROTATION_0:
            madctl |= ST7789_MADCTL_MX | ST7789_MADCTL_MV;
            s_width  = ST7789_RAM_HEIGHT;
            s_height = ST7789_RAM_WIDTH;
            break;
        case DISPLAY_ROTATION_90:
            madctl |= 0x00;
            s_width  = ST7789_RAM_WIDTH;
            s_height = ST7789_RAM_HEIGHT;
            break;
        case DISPLAY_ROTATION_180:
            madctl |= ST7789_MADCTL_MY | ST7789_MADCTL_MV;
            s_width  = ST7789_RAM_HEIGHT;
            s_height = ST7789_RAM_WIDTH;
            break;
        case DISPLAY_ROTATION_270:
            madctl |= ST7789_MADCTL_MX | ST7789_MADCTL_MY;
            s_width  = ST7789_RAM_WIDTH;
            s_height = ST7789_RAM_HEIGHT;
            break;
        default:
            return DISPLAY_ERROR;
    }

    write_command(ST7789_CMD_MADCTL);
    write_data_u8(madctl);

    s_rotation = rotation;
    return DISPLAY_OK;
}

void Display_GetSize(uint16_t *width, uint16_t *height)
{
    if (width)  *width  = s_width;
    if (height) *height = s_height;
}

Display_Status_t Display_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }
    if (x1 >= s_width || y1 >= s_height || x0 > x1 || y0 > y1) {
        return DISPLAY_ERROR;
    }

    uint8_t caset[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
                         (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    uint8_t raset[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
                         (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };

    write_command(ST7789_CMD_CASET);
    write_data(caset, sizeof(caset));

    write_command(ST7789_CMD_RASET);
    write_data(raset, sizeof(raset));

    write_command(ST7789_CMD_RAMWR);
    return DISPLAY_OK;
}

Display_Status_t Display_WritePixelsDMA(const display_color_t *pixels, uint32_t count)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }
    if (pixels == NULL || count == 0) {
        return DISPLAY_ERROR;
    }

    s_fill_remaining = 0;

    uint32_t chunk = (count > DISPLAY_MAX_DMA_PIXELS) ? DISPLAY_MAX_DMA_PIXELS : count;
    s_write_ptr       = pixels + chunk;
    s_write_remaining = count - chunk;

    s_busy = true;
    dc_data();

    if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)pixels, chunk * sizeof(display_color_t)) != HAL_OK) {
        s_busy = false;
        s_write_remaining = 0;
        return DISPLAY_ERROR;
    }
    return DISPLAY_OK;
}

Display_Status_t Display_FillColorDMA(display_color_t color, uint32_t count)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }
    if (count == 0) {
        return DISPLAY_ERROR;
    }

    for (uint32_t i = 0; i < FILL_LINE_PIXELS; i++) {
        s_fill_line[i] = color;
    }
    s_fill_color = color;
    s_write_remaining = 0;

    uint32_t chunk = (count > FILL_LINE_PIXELS) ? FILL_LINE_PIXELS : count;
    s_fill_remaining = count - chunk;

    s_busy = true;
    dc_data();

    if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)s_fill_line, chunk * sizeof(display_color_t)) != HAL_OK) {
        s_busy = false;
        s_fill_remaining = 0;
        return DISPLAY_ERROR;
    }
    return DISPLAY_OK;
}

Display_Status_t Display_SleepIn(void)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }
    write_command(ST7789_CMD_SLPIN);
    return DISPLAY_OK;
}

Display_Status_t Display_SleepOut(void)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }
    write_command(ST7789_CMD_SLPOUT);
    HAL_Delay(ST7789_DELAY_SLPOUT_MS);
    return DISPLAY_OK;
}
