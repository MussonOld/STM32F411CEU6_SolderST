/**
 * @file    screen_icons.c
 * @brief   Растровые иконки экрана — см. screen_icons.h.
 */
#include "screen_icons.h"
#include "screen_layout.h"
#include "display.h"
#include "sleep_icon.h"
#include "presleep_icon.h"
#include "solder_icon.h"
#include "desolder_icon.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Битмап иконки "циферблат", 14x14, 1 бит/пиксель (MSB=левый пиксель
 *        строки) — окружность + часовая/минутная стрелки. Нарисован вручную
 *        (не из шрифта — шрифты проекта не содержат символ часов/циферблата,
 *        см. bdf2c_TFT.py/fonts.h).
 */
static const uint16_t s_sleep_icon_bitmap[SLEEP_ICON_H] = {
    0b00011111110000,
    0b00111000111000,
    0b01100000001100,
    0b11000010000110,
    0b11000010000110,
    0b10000010000010,
    0b10000011000011,
    0b10000011100010,
    0b11000000110110,
    0b11000000000110,
    0b01100000001100,
    0b00111000111000,
    0b00011111110000,
    0b00000000000000,
};

/**
 * @brief Нарисовать иконку циферблата (статично, блокирующе — как и
 *        draw_divider(): редкое событие, старт/смена режима экрана, не
 *        каждый кадр). Буфер на стеке — функция дожидается завершения DMA
 *        перед возвратом, буфер не переживёт функцию иначе.
 *
 * @return true, если реально нарисована (Display_SetWindow() и запуск DMA
 *         успешны). false — окно не выставилось или DMA не стартовал
 *         (DISPLAY_ERROR); иконка не нарисована (или нарисована частично). Вызывающий код (update_sleep_status()) ОБЯЗАН
 *         проверять результат и не фиксировать s_sleep_icon_shown[]/
 *         s_sleep_icon_x[] как "нарисовано", если он false — иначе
 *         состояние разъезжается с реальным экраном НАВСЕГДА (следующий
 *         вызов решит, что иконка уже там, где её на самом деле нет, и
 *         не предпримет повторной попытки).
 */
bool ScreenIcons_DrawClock(uint16_t x, uint16_t y)
{
    display_color_t buf[SLEEP_ICON_W * SLEEP_ICON_H];

    for (uint16_t row = 0; row < SLEEP_ICON_H; row++) {
        uint16_t bits = s_sleep_icon_bitmap[row];
        for (uint16_t col = 0; col < SLEEP_ICON_W; col++) {
            bool on = ((bits >> (SLEEP_ICON_W - 1U - col)) & 0x1U) != 0U;
            buf[(row * SLEEP_ICON_W) + col] = on ? COLOR_SLEEP_ICON : COLOR_BG;
        }
    }

    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SLEEP_ICON_W - 1U), (uint16_t)(y + SLEEP_ICON_H - 1U)) != DISPLAY_OK) {
        return false;
    }
    if (Display_WritePixelsDMA(buf, (uint32_t)SLEEP_ICON_W * SLEEP_ICON_H) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Стереть иконку циферблата (залить фоном) — когда таймер не
 *        отображается (см. update_sleep_status()). Тот же блокирующий
 *        паттерн, что и ScreenIcons_DrawClock() — редкое событие (смена
 *        видимости), не каждый кадр.
 *
 * @return true, если реально стёрта — тот же контракт и та же причина,
 *         что и у ScreenIcons_DrawClock() (см. её докстринг).
 */
bool ScreenIcons_EraseClock(uint16_t x, uint16_t y)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SLEEP_ICON_W - 1U), (uint16_t)(y + SLEEP_ICON_H - 1U)) != DISPLAY_OK) {
        return false;
    }
    if (Display_FillColorDMA(COLOR_BG, (uint32_t)SLEEP_ICON_W * SLEEP_ICON_H) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Крестик "инструмент не подключен" (CHANNEL_CONTENT_IDLE) —
 *        квадрат SCREEN_IDLE_CROSS_SIZE, две диагонали толщиной
 *        2*SCREEN_IDLE_CROSS_THICKNESS+1. В отличие от циферблата (см.
 *        s_sleep_icon_bitmap), тут нет готовой битовой сетки — тест "у
 *        диагонали" считается построчно, в СТАТИЧЕСКИЙ (не стековый) буфер
 *        на одну строку: полный буфер 48x48 пикселей (4.6 КБ) на стеке — в
 *        одном кадре с main()/Screen_Update() и без RTOS многовато для стека, одна строка (96 байт) безопасна.
 *        Блокирующий построчный вывод — редкое событие (смена
 *        видимости), не каждый кадр, как и у циферблата.
 */
/** @brief |a - b| <= t — точка (a, b) лежит у диагонали толщиной 2*t+1. */
static bool near_diagonal(uint16_t a, uint16_t b, uint16_t t)
{
    return (uint16_t)((a > b) ? (a - b) : (b - a)) <= t;
}

/** @brief Пиксели одной строки крестика (две диагонали) в row_buf[SCREEN_IDLE_CROSS_SIZE]. */
static void fill_idle_cross_row(display_color_t *row_buf, uint16_t row)
{
    const uint16_t size = SCREEN_IDLE_CROSS_SIZE;
    const uint16_t t = SCREEN_IDLE_CROSS_THICKNESS;

    for (uint16_t col = 0; col < size; col++) {
        uint16_t anti_col = (uint16_t)(size - 1U - col);
        bool on_diag = near_diagonal(row, col, t) || near_diagonal(row, anti_col, t);
        row_buf[col] = on_diag ? COLOR_IDLE_CROSS : COLOR_BG;
    }
}

bool ScreenIcons_DrawIdleCross(uint16_t x, uint16_t y)
{
    static display_color_t s_row_buf[SCREEN_IDLE_CROSS_SIZE]; /* см. докстринг — не на стеке */
    const uint16_t size = SCREEN_IDLE_CROSS_SIZE;

    while (Display_IsBusy()) { }
    for (uint16_t row = 0; row < size; row++) {
        fill_idle_cross_row(s_row_buf, row);
        if (Display_SetWindow(x, (uint16_t)(y + row), (uint16_t)(x + size - 1U), (uint16_t)(y + row)) != DISPLAY_OK) {
            return false;
        }
        if (Display_WritePixelsDMA(s_row_buf, size) != DISPLAY_OK) {
            return false;
        }
        while (Display_IsBusy()) { }
    }
    return true;
}

/** @brief Стереть крестик (залить фоном) — тот же контракт, что и ScreenIcons_EraseClock(). */
bool ScreenIcons_EraseIdleCross(uint16_t x, uint16_t y)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + SCREEN_IDLE_CROSS_SIZE - 1U),
                           (uint16_t)(y + SCREEN_IDLE_CROSS_SIZE - 1U)) != DISPLAY_OK) {
        return false;
    }
    if (Display_FillColorDMA(COLOR_BG, (uint32_t)SCREEN_IDLE_CROSS_SIZE * SCREEN_IDLE_CROSS_SIZE) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Нарисовать готовый растр RGB565 (w x h, построчно) в (x, y) —
 *        общий блокирующий проход для иконок SLEEP/PRESLEEP (редкое событие
 *        — смена фазы сна, не каждый кадр). Данные уже готовы построчно в
 *        Flash, буфер (ни стековый, ни статический) не нужен, пишем прямо из
 *        константного массива одним DMA-проходом.
 *
 * @return true, если реально нарисован — вызывающий фиксирует "показана"
 *         (s_asleep_icon_shown[]/s_presleep_icon_shown[]) только по true,
 *         иначе на следующем кадре попробует снова (см. ScreenIcons_DrawIdleCross()).
 */
static bool draw_raster(const uint16_t *bitmap, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U)) != DISPLAY_OK) {
        return false;
    }
    if (Display_WritePixelsDMA(bitmap, (uint32_t)w * h) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { }
    return true;
}

/** @brief Залить прямоугольник фоном — общий стиратель для растровых иконок (тот же контракт, что и у draw_raster()). */
static bool erase_raster(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U)) != DISPLAY_OK) {
        return false;
    }
    if (Display_FillColorDMA(COLOR_BG, (uint32_t)w * h) != DISPLAY_OK) {
        return false;
    }
    while (Display_IsBusy()) { }
    return true;
}

/**
 * @brief Нарисовать иконку SLEEP (спящий смайлик, SleepIcon_Bitmap из
 *        sleep_icon.h/.c). Контракт возврата — как у draw_raster().
 */
bool ScreenIcons_DrawAsleep(uint16_t x, uint16_t y)
{
    return draw_raster(SleepIcon_Bitmap, x, y, SLEEP_ICON_BITMAP_W, SLEEP_ICON_BITMAP_H);
}

/** @brief Стереть иконку SLEEP (залить фоном) — тот же контракт, что и ScreenIcons_EraseIdleCross(). */
bool ScreenIcons_EraseAsleep(uint16_t x, uint16_t y)
{
    return erase_raster(x, y, SLEEP_ICON_BITMAP_W, SLEEP_ICON_BITMAP_H);
}

/**
 * @brief Нарисовать иконку PRESLEEP (зевающий смайлик, PreSleepIcon_Bitmap
 *        из presleep_icon.h/.c) — тот же блокирующий паттерн и тот же
 *        контракт возврата, что и ScreenIcons_DrawAsleep() (см. её докстринг и
 *        s_presleep_icon_shown). Фон растра уже чёрный (COLOR_BG), поэтому
 *        рисуется прямоугольником целиком; стирание — ScreenIcons_ErasePresleep().
 */
bool ScreenIcons_DrawPresleep(uint16_t x, uint16_t y)
{
    return draw_raster(PreSleepIcon_Bitmap, x, y, PRESLEEP_ICON_BITMAP_W, PRESLEEP_ICON_BITMAP_H);
}

/** @brief Стереть иконку PRESLEEP (залить фоном) — весь прямоугольник растра. */
bool ScreenIcons_ErasePresleep(uint16_t x, uint16_t y)
{
    return erase_raster(x, y, PRESLEEP_ICON_BITMAP_W, PRESLEEP_ICON_BITMAP_H);
}

/**
 * @brief Нарисовать иконки заголовков ("Паяльник"/"Отсос", см.
 *        solder_icon.h/desolder_icon.h) — статичны, без erase-пары: в
 *        отличие от иконок IDLE/SLEEP (зависят от состояния канала и
 *        стираются/перерисовываются по ходу работы), эти рисуются только
 *        два раза за всё время — в Screen_Init() и повторно в
 *        clear_screen_for_mode_switch() после полной заливки экрана при
 *        возврате из сервисного меню (см. её докстринг). Тот же блокирующий
 *        паттерн одним DMA-проходом, что и ScreenIcons_DrawAsleep().
 */
void ScreenIcons_DrawSolderTitle(void)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(SCREEN_SOLDER_ICON_X, SCREEN_SOLDER_ICON_Y,
                           (uint16_t)(SCREEN_SOLDER_ICON_X + SOLDER_ICON_BITMAP_W - 1U),
                           (uint16_t)(SCREEN_SOLDER_ICON_Y + SOLDER_ICON_BITMAP_H - 1U)) != DISPLAY_OK) {
        return;
    }
    Display_WritePixelsDMA(SolderIcon_Bitmap, (uint32_t)SOLDER_ICON_BITMAP_W * SOLDER_ICON_BITMAP_H);
    while (Display_IsBusy()) { }
}

/** @brief Нарисовать иконку заголовка "Отсос" — тот же контракт, что и ScreenIcons_DrawSolderTitle(). */
void ScreenIcons_DrawDesolderTitle(void)
{
    while (Display_IsBusy()) { }
    if (Display_SetWindow(SCREEN_DESOLDER_ICON_X, SCREEN_DESOLDER_ICON_Y,
                           (uint16_t)(SCREEN_DESOLDER_ICON_X + DESOLDER_ICON_BITMAP_W - 1U),
                           (uint16_t)(SCREEN_DESOLDER_ICON_Y + DESOLDER_ICON_BITMAP_H - 1U)) != DISPLAY_OK) {
        return;
    }
    Display_WritePixelsDMA(DesolderIcon_Bitmap, (uint32_t)DESOLDER_ICON_BITMAP_W * DESOLDER_ICON_BITMAP_H);
    while (Display_IsBusy()) { }
}
