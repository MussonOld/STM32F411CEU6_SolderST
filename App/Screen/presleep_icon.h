/**
 * @file presleep_icon.h
 * @brief Растр иконки PRESLEEP (зевающий смайлик), RGB565, 70x80.
 *        См. presleep_icon.c — сгенерирован из Icons/PreSleepy.png с
 *        очисткой белого фона (на чёрном фоне экрана без ореола).
 */

#ifndef APP_SCREEN_PRESLEEP_ICON_H_INCLUDED
#define APP_SCREEN_PRESLEEP_ICON_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define PRESLEEP_ICON_BITMAP_W (70U)
#define PRESLEEP_ICON_BITMAP_H (80U)

/** Пиксели RGB565, построчно (row-major), 70x80 = 5600 элементов. */
extern const uint16_t PreSleepIcon_Bitmap[PRESLEEP_ICON_BITMAP_W * PRESLEEP_ICON_BITMAP_H];

#ifdef __cplusplus
}
#endif

#endif /* APP_SCREEN_PRESLEEP_ICON_H_INCLUDED */
