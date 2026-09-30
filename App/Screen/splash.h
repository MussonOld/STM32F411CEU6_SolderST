/**
 * @file splash.h
 * @brief Растр заставки при включении (Icons/Designer.png), RGB565, 320x240.
 */

#ifndef APP_SCREEN_SPLASH_H_INCLUDED
#define APP_SCREEN_SPLASH_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SPLASH_BITMAP_W (320U)
#define SPLASH_BITMAP_H (240U)

/** Сколько держать заставку на экране при включении, мс. */
#define SPLASH_HOLD_MS (2000U)

/** Пиксели RGB565, построчно (row-major), 320x240 = 76800 элементов. */
extern const uint16_t SplashScreen_Bitmap[SPLASH_BITMAP_W * SPLASH_BITMAP_H];

#ifdef __cplusplus
}
#endif

#endif /* APP_SCREEN_SPLASH_H_INCLUDED */
