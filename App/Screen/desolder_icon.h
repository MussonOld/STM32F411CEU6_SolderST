/**
 * @file desolder_icon.h
 * @brief Растр иконки заголовка (отсос, заголовок Desolder — фон исходника уже чёрный (JPEG), альфа не нужна), RGB565, 120x31.
 */

#ifndef APP_SCREEN_DESOLDER_ICON_H_INCLUDED
#define APP_SCREEN_DESOLDER_ICON_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define DESOLDER_ICON_BITMAP_W (120U)
#define DESOLDER_ICON_BITMAP_H (31U)

/** Пиксели RGB565, построчно (row-major), 120x31 = 3720 элементов. */
extern const uint16_t DesolderIcon_Bitmap[DESOLDER_ICON_BITMAP_W * DESOLDER_ICON_BITMAP_H];

#ifdef __cplusplus
}
#endif

#endif /* APP_SCREEN_DESOLDER_ICON_H_INCLUDED */
