/**
 * @file ili9341.c
 * @brief Реализация display.h для контроллера ILI9341
 *
 * SPI1 (TX-only + DMA2_Stream2), CS — аппаратно на GND (не управляется программно).
 * DC = PA3 (Disp_DC), RST = PA4 (Disp_RST) — те же пины, что и у st7789.c
 * (одна и та же плата, сменилась только панель).
 *
 * ВАЖНО: этот файл и st7789.c реализуют один и тот же контракт (display.h)
 * и оба переопределяют HAL_SPI_TxCpltCallback() — собирать в проект нужно
 * ТОЛЬКО ОДИН из них одновременно (выбор контроллера на этапе компиляции,
 * см. докстринг display.h/Display.md). В CubeIDE: правой кнопкой на
 * неиспользуемый файл -> Resource Configurations -> Exclude from Build...
 * для всех конфигураций, либо просто убрать его из проекта/переместить вне
 * дерева исходников.
 */

#include "display.h"
#include "ili9341.h"
#include "stm32f4xx_hal.h"
#include "main.h"
#include <string.h>

extern SPI_HandleTypeDef hspi1;

/* Максимум пикселей за один DMA-транш: HAL_SPI_Transmit_DMA принимает Size
 * как uint16_t (макс. 65535 байт = 32767 пикселей по 2 байта). Оба чанковых
 * пути (fill: FILL_LINE_PIXELS=320, write: DISPLAY_WRITE_CHUNK_PIXELS=512,
 * см. ниже) с большим запасом укладываются в этот предел сами по себе —
 * отдельная константа-ограничитель на транш здесь не нужна. */

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

/* Продолжение чанкованной передачи внешнего буфера пикселей (WritePixelsDMA).
 * s_write_ptr — указатель на буфер ВЫЗЫВАЮЩЕГО КОДА, он const и НЕ мутируется
 * (см. байт-свап ниже — идёт в собственный scratch-буфер модуля, а не на месте). */
static const display_color_t *s_write_ptr = NULL;
static uint32_t s_write_remaining = 0;

/* Scratch-буфер под байт-свап одного чанка перед отправкой по DMA. Размер —
 * компромисс между частотой рестартов DMA (чем меньше буфер, тем чаще) и
 * расходом RAM; для UI-элементов (текст/иконки) этого с большим запасом
 * хватает за 1-2 чанка, для будущих больших заливок изображений — просто
 * больше рестартов DMA, не более. */
#define DISPLAY_WRITE_CHUNK_PIXELS  512U
static display_color_t s_write_scratch[DISPLAY_WRITE_CHUNK_PIXELS];

/* ---- Низкоуровневые примитивы ---- */

static inline void dc_command(void) { HAL_GPIO_WritePin(Disp_DC_GPIO_Port, Disp_DC_Pin, GPIO_PIN_RESET); }
static inline void dc_data(void)    { HAL_GPIO_WritePin(Disp_DC_GPIO_Port, Disp_DC_Pin, GPIO_PIN_SET); }

/**
 * @brief Развернуть байты 16-бит цвета для передачи по SPI
 *
 * ILI9341 в режиме RAMWR/16bpp, как и ST7789, ждёт каждый пиксель СТАРШИМ
 * байтом вперёд. STM32 — little-endian, поэтому display_color_t в памяти
 * лежит младшим байтом первым; передача "как есть" (прямым приведением к
 * uint8_t*) отправляет байты в обратном порядке — R и B частично
 * переставляются местами (например, чистый жёлтый 0xFE80 доходит до
 * контроллера как приглушённый синий).
 */
static inline display_color_t byte_swap16(display_color_t v)
{
    return (display_color_t)((v << 8) | (v >> 8));
}

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

/**
 * @brief Развернуть байты очередного чанка из буфера вызывающего кода
 *        (s_write_ptr, const, НЕ мутируется) в собственный scratch-буфер
 *        драйвера и запустить его передачу по DMA. Продвигает s_write_ptr/
 *        s_write_remaining. Вызывается и из Display_WritePixelsDMA() (первый
 *        чанк), и из ILI9341_OnDmaTxComplete() (продолжение).
 */
static Display_Status_t start_write_chunk(void)
{
    uint32_t chunk = (s_write_remaining > DISPLAY_WRITE_CHUNK_PIXELS) ? DISPLAY_WRITE_CHUNK_PIXELS
                                                                        : s_write_remaining;
    for (uint32_t i = 0; i < chunk; i++) {
        s_write_scratch[i] = byte_swap16(s_write_ptr[i]);
    }

    dc_data();
    if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)s_write_scratch, chunk * sizeof(display_color_t)) != HAL_OK) {
        return DISPLAY_ERROR; /* s_write_ptr/s_write_remaining НЕ продвигаем — чанк не ушёл */
    }

    s_write_ptr       += chunk;
    s_write_remaining -= chunk;
    return DISPLAY_OK;
}

/* ---- Обработчик завершения DMA (вызывается из HAL_SPI_TxCpltCallback) ---- */

void ILI9341_OnDmaTxComplete(void)
{
    if (s_fill_remaining > 0) {
        uint32_t chunk = (s_fill_remaining > FILL_LINE_PIXELS) ? FILL_LINE_PIXELS : s_fill_remaining;
        dc_data();
        if (HAL_SPI_Transmit_DMA(&hspi1, (uint8_t *)s_fill_line, chunk * sizeof(display_color_t)) != HAL_OK) {
            s_busy = false; /* DMA не смогла продолжить передачу — останавливаемся, не зависаем в busy навсегда */
            return;
        }
        s_fill_remaining -= chunk; /* декремент только при подтверждённом старте DMA */
        return;
    }

    if (s_write_remaining > 0) {
        if (start_write_chunk() != DISPLAY_OK) {
            s_busy = false; /* DMA не смогла продолжить передачу — останавливаемся, не зависаем в busy навсегда */
        }
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
        ILI9341_OnDmaTxComplete();
    }
}

/* ---- Реализация контракта display.h ---- */

Display_Status_t Display_Init(void)
{
    HAL_GPIO_WritePin(Disp_RST_GPIO_Port, Disp_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(ILI9341_DELAY_RESET_MS);
    HAL_GPIO_WritePin(Disp_RST_GPIO_Port, Disp_RST_Pin, GPIO_PIN_SET);
    HAL_Delay(ILI9341_DELAY_AFTER_RST_MS);

    /* Полная init-последовательность (типовая для модулей на ILI9341 —
     * Power Control/VCOM/Gamma; см. комментарий в ili9341.h про то, почему
     * ST7789-драйвер с одним SLPOUT+COLMOD запускал эту панель лишь
     * "иногда"). Значения — стандартные заводские из применяемых на таких
     * модулях референсных init-последовательностей; если после прошивки
     * контраст/цвета выглядят не оптимально — подбирается по месту,
     * контроллер от неверных (в разумных пределах) значений не "залипает". */
    write_command(ILI9341_CMD_PWCTRB);
    { uint8_t d[3] = {0x00, 0xC1, 0x30}; write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_POSC);
    { uint8_t d[4] = {0x64, 0x03, 0x12, 0x81}; write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_DRVTIMA);
    { uint8_t d[3] = {0x85, 0x00, 0x78}; write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_PWSEQCTRL);
    write_data_u8(0x20);

    write_command(ILI9341_CMD_DRVTIMB);
    { uint8_t d[2] = {0x00, 0x00}; write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_PWCTR1);
    write_data_u8(0x23); /* GVDD ~4.6V */

    write_command(ILI9341_CMD_PWCTR2);
    write_data_u8(0x10); /* Step-up factor */

    write_command(ILI9341_CMD_VMCTR1);
    { uint8_t d[2] = {0x3E, 0x28}; write_data(d, sizeof(d)); } /* VCOMH/VCOML */

    write_command(ILI9341_CMD_VMCTR2);
    write_data_u8(0x86); /* VCOM offset */

    write_command(ILI9341_CMD_FRMCTR1);
    { uint8_t d[2] = {0x00, 0x18}; write_data(d, sizeof(d)); } /* ~79Hz, диапазон по умолчанию */

    write_command(ILI9341_CMD_DFUNCTR);
    { uint8_t d[3] = {0x08, 0x82, 0x27}; write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_EN3GAM);
    write_data_u8(0x00); /* 3G (доп. гамма-режим) выключен */

    write_command(ILI9341_CMD_GAMSET);
    write_data_u8(0x01); /* гамма-кривая 1 */

    write_command(ILI9341_CMD_GMCTRP1);
    { uint8_t d[15] = {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E,
                        0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00};
      write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_GMCTRN1);
    { uint8_t d[15] = {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31,
                        0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F};
      write_data(d, sizeof(d)); }

    write_command(ILI9341_CMD_COLMOD);
    write_data_u8(ILI9341_COLMOD_16BPP);

    write_command(ILI9341_CMD_SLPOUT);
    HAL_Delay(ILI9341_DELAY_SLPOUT_MS);

    Display_SetRotation(DISPLAY_ROTATION_0);

    write_command(ILI9341_CMD_INVOFF); /* ILI9341, в отличие от ST7789, обычно НЕ требует INVON —
                                         * если цвета выглядят инвертированными на этой конкретной
                                         * панели, заменить на ILI9341_CMD_INVON по месту */
    write_command(ILI9341_CMD_NORON);
    write_command(ILI9341_CMD_DISPON);

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

    /* MX/MY здесь ЗЕРКАЛЬНО по отношению к тому, что было в st7789.c: на
     * ILI9341 при том же MV (row/col exchange) картинка выходила зеркальной
     * по горизонтали при исходной паре MX|MV/MY|MV — поменял местами MX<->MY
     * во всех четырёх ориентациях. Проверено вживую только для ROTATION_0
     * (единственная, которую реально использует Display_Init()); 90/180/270
     * сейчас не задействуются нигде в коде — если потребуются, картинка от
     * них может оказаться зеркальной или повёрнутой не в ту сторону,
     * проверять на месте. */
    uint8_t madctl = ILI9341_MADCTL_RGB; /* поменять на ILI9341_MADCTL_BGR, если R/B перепутаны на этой панели */

    switch (rotation) {
        case DISPLAY_ROTATION_0:
            madctl |= ILI9341_MADCTL_MY | ILI9341_MADCTL_MV;
            s_width  = ILI9341_RAM_HEIGHT;
            s_height = ILI9341_RAM_WIDTH;
            break;
        case DISPLAY_ROTATION_90:
            madctl |= ILI9341_MADCTL_MX | ILI9341_MADCTL_MY;
            s_width  = ILI9341_RAM_WIDTH;
            s_height = ILI9341_RAM_HEIGHT;
            break;
        case DISPLAY_ROTATION_180:
            madctl |= ILI9341_MADCTL_MX | ILI9341_MADCTL_MV;
            s_width  = ILI9341_RAM_HEIGHT;
            s_height = ILI9341_RAM_WIDTH;
            break;
        case DISPLAY_ROTATION_270:
            madctl |= 0x00;
            s_width  = ILI9341_RAM_WIDTH;
            s_height = ILI9341_RAM_HEIGHT;
            break;
        default:
            return DISPLAY_ERROR;
    }

    write_command(ILI9341_CMD_MADCTL);
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

    write_command(ILI9341_CMD_CASET);
    write_data(caset, sizeof(caset));

    write_command(ILI9341_CMD_RASET);
    write_data(raset, sizeof(raset));

    write_command(ILI9341_CMD_RAMWR);
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

    s_fill_remaining  = 0;
    s_write_ptr       = pixels;
    s_write_remaining = count;

    s_busy = true;
    if (start_write_chunk() != DISPLAY_OK) { /* байт-свап первого чанка в scratch + запуск DMA; pixels не трогается */
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

    display_color_t wire_color = byte_swap16(color); /* см. byte_swap16() выше */
    for (uint32_t i = 0; i < FILL_LINE_PIXELS; i++) {
        s_fill_line[i] = wire_color;
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
    write_command(ILI9341_CMD_SLPIN);
    return DISPLAY_OK;
}

Display_Status_t Display_SleepOut(void)
{
    if (s_busy) {
        return DISPLAY_BUSY;
    }
    write_command(ILI9341_CMD_SLPOUT);
    HAL_Delay(ILI9341_DELAY_SLPOUT_MS);
    return DISPLAY_OK;
}
