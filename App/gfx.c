/**
 * @file gfx.h
 * @brief Графические примитивы поверх display.h. Не знает про SPI/DMA/ST7789 —
 *        только про пиксели через контракт display.h.
 */

#ifndef GFX_H
#define GFX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "display.h"
#include "fonts.h"

typedef enum {
    GFX_OK = 0,
    GFX_ERROR
} Gfx_Status_t;

/**
 * @brief Нарисовать строку текста (UTF-8) заданным шрифтом
 * @param x, y       Верхний левый угол бокса шрифта (не baseline)
 * @param text       Строка в кодировке UTF-8 (поддержка ASCII + кириллица — по лексикону шрифта)
 * @param font       Шрифт (см. fonts.h)
 * @param fg_color   Цвет символов
 * @param bg_color   Цвет фона (заливается непрозрачно под каждым символом)
 * @return Ширина отрисованного текста в пикселях (сумма dwidth), либо 0 при ошибке
 */
uint16_t Gfx_DrawText(uint16_t x, uint16_t y, const char *text,
                       const font_t *font,
                       display_color_t fg_color, display_color_t bg_color);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */