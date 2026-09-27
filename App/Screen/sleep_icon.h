/**
 * @file sleep_icon.h
 * @brief Растр иконки SLEEP (спящий смайлик в колпаке), RGB565, 70x66.
 *        См. sleep_icon.c — сгенерирован из PNG, вписан в 70x70 с
 *        сохранением пропорций исходника (231x218).
 */

#ifndef SLEEP_ICON_H
#define SLEEP_ICON_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SLEEP_ICON_BITMAP_W (70U)
#define SLEEP_ICON_BITMAP_H (66U)

/** Пиксели RGB565, построчно (row-major), 70x66 = 4620 элементов. */
extern const uint16_t SleepIcon_Bitmap[SLEEP_ICON_BITMAP_W * SLEEP_ICON_BITMAP_H];

#ifdef __cplusplus
}
#endif

#endif /* SLEEP_ICON_H */
