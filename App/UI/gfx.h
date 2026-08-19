/**
 * @file gfx.h
 * @brief Графические примитивы поверх display.h. Не знает про SPI/DMA/ST7789 —
 *        только про пиксели через контракт display.h.
 *
 * Неблокирующий интерфейс: Gfx_DrawTextStart() ставит задание в очередь,
 * Gfx_Process() продвигает его на один символ за вызов, если DMA свободен.
 * Ни одна функция не ждёт DMA — вызывающий код (главный цикл с PID/safety)
 * никогда не зависит от скорости SPI-дисплея.
 */

#ifndef GFX_H
#define GFX_H

#ifdef __cplusplus
extern "C" {
#endif

#include "display.h"
#include "fonts.h"

typedef enum {
    GFX_JOB_IDLE = 0,  /**< Нет активного задания */
    GFX_JOB_BUSY,      /**< Задание выполняется (ещё есть символы для отрисовки) */
    GFX_JOB_DONE,       /**< Задание завершено на предыдущем вызове Gfx_Process() */
    GFX_JOB_ERROR       /**< Некорректные аргументы при постановке задания */
} Gfx_JobState_t;

/**
 * @brief Поставить текст в очередь на отрисовку. Не блокирует.
 * @param x, y       Верхний левый угол бокса шрифта (не baseline)
 * @param text       Строка в кодировке UTF-8
 * @param font       Шрифт (см. fonts.h)
 * @param fg_color   Цвет символов
 * @param bg_color   Цвет фона (заливается непрозрачно под каждым символом)
 * @return GFX_JOB_BUSY при успешной постановке (или если уже есть незавершённое
 *         задание — новое отклоняется), GFX_JOB_ERROR при неверных аргументах
 */
Gfx_JobState_t Gfx_DrawTextStart(uint16_t x, uint16_t y, const char *text,
                                  const font_t *font,
                                  display_color_t fg_color, display_color_t bg_color);

/**
 * @brief Продвинуть текущее задание отрисовки на один символ.
 *
 * Вызывать из главного цикла на каждой итерации. Если DMA ещё занят
 * предыдущим символом — функция сразу возвращает управление (не ждёт).
 *
 * @return Текущее состояние задания после попытки продвижения
 */
Gfx_JobState_t Gfx_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */
