/**
 * @file    screen_icons.h
 * @brief   Растровые иконки экрана (App/Screen, внутренний модуль): циферблат
 *          перед таймером сна, крестик "инструмент не подключен", смайлики
 *          SLEEP/PRESLEEP и иконки заголовков каналов.
 *
 * Всё рисуется СЫРЫМ Display_SetWindow/Display_WritePixelsDMA в обход TextField
 * (TextField про эти пиксели не знает и не стирает их сам), блокирующе — на
 * редких событиях (смена видимости), не каждый кадр. Draw/Erase возвращают
 * true при успехе, false — если окно/DMA не приняли (состояние "нарисовано"
 * вызывающий фиксирует только по true). Координаты — левый верхний угол.
 */
#ifndef SCREEN_ICONS_H
#define SCREEN_ICONS_H

#include <stdbool.h>
#include <stdint.h>

bool ScreenIcons_DrawClock(uint16_t x, uint16_t y);
bool ScreenIcons_EraseClock(uint16_t x, uint16_t y);

bool ScreenIcons_DrawIdleCross(uint16_t x, uint16_t y);
bool ScreenIcons_EraseIdleCross(uint16_t x, uint16_t y);

bool ScreenIcons_DrawAsleep(uint16_t x, uint16_t y);
bool ScreenIcons_EraseAsleep(uint16_t x, uint16_t y);

bool ScreenIcons_DrawPresleep(uint16_t x, uint16_t y);
bool ScreenIcons_ErasePresleep(uint16_t x, uint16_t y);

/** Иконки заголовков каналов ("Паяльник"/"Отсос") — на фиксированных местах, блокирующе. */
void ScreenIcons_DrawSolderTitle(void);
void ScreenIcons_DrawDesolderTitle(void);

#endif /* SCREEN_ICONS_H */
