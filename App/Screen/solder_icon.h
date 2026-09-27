/**
 * @file solder_icon.h
 * @brief Растр иконки заголовка (паяльник, заголовок Solder), RGB565, 150x21.
 */

#ifndef APP_SCREEN_SOLDER_ICON_H_INCLUDED
#define APP_SCREEN_SOLDER_ICON_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SOLDER_ICON_BITMAP_W (150U)
#define SOLDER_ICON_BITMAP_H (21U)

/** Пиксели RGB565, построчно (row-major), 150x21 = 3150 элементов. */
extern const uint16_t SolderIcon_Bitmap[SOLDER_ICON_BITMAP_W * SOLDER_ICON_BITMAP_H];

#ifdef __cplusplus
}
#endif

#endif /* APP_SCREEN_SOLDER_ICON_H_INCLUDED */
