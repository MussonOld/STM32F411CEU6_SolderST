/**
 * @file desolder_icon.h
 * @brief Растр иконки заголовка (отсос, заголовок Desolder — фон исходника уже чёрный (JPEG), альфа не нужна), RGB565, 150x39.
 */

#ifndef APP_SCREEN_DESOLDER_ICON_H_INCLUDED
#define APP_SCREEN_DESOLDER_ICON_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define DESOLDER_ICON_BITMAP_W (150U)
#define DESOLDER_ICON_BITMAP_H (39U)

/** Пиксели RGB565, построчно (row-major), 150x39 = 5850 элементов. */
extern const uint16_t DesolderIcon_Bitmap[DESOLDER_ICON_BITMAP_W * DESOLDER_ICON_BITMAP_H];

#ifdef __cplusplus
}
#endif

#endif /* APP_SCREEN_DESOLDER_ICON_H_INCLUDED */
