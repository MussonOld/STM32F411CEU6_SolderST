/**
 * @file display.h
 * @brief Общий контракт драйвера дисплея (независим от конкретного контроллера)
 *
 * Реализуется отдельно для каждого чипа (st7789.c, ili9341.c — выбор на этапе
 * компиляции). Верхний слой (графика/UI) инклюдит только этот файл и не имеет
 * представления о том, какой контроллер используется физически.
 *
 * Формат пикселя — RGB565 (16 бит), как того требует протокол ST7789/ILI9341.
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Код результата операции драйвера
 */
typedef enum {
    DISPLAY_OK = 0,     /**< Операция выполнена / поставлена в очередь успешно */
    DISPLAY_BUSY,       /**< Предыдущая асинхронная передача ещё не завершена */
    DISPLAY_ERROR        /**< Ошибка на уровне периферии (SPI/DMA) */
} Display_Status_t;

/**
 * @brief Ориентация экрана
 */
typedef enum {
    DISPLAY_ROTATION_0 = 0,
    DISPLAY_ROTATION_90,
    DISPLAY_ROTATION_180,
    DISPLAY_ROTATION_270
} Display_Rotation_t;

/**
 * @brief Цвет пикселя в формате RGB565
 */
typedef uint16_t display_color_t;

/**
 * @brief Собрать RGB565 из компонентов 8-бит R/G/B
 */
#define DISPLAY_RGB565(r, g, b) \
    ((display_color_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

/**
 * @brief Callback, вызываемый по завершении асинхронной DMA-передачи пикселей
 *
 * Вызывается из контекста прерывания (HAL_SPI_TxCpltCallback). Должен быть
 * коротким — не блокировать, не дергать SPI повторно изнутри самого callback'а.
 */
typedef void (*Display_TxCpltCallback_t)(void);

/**
 * @brief Инициализировать дисплей (аппаратный reset + init-последовательность контроллера)
 * @return DISPLAY_OK при успехе, DISPLAY_ERROR при сбое инициализации
 */
Display_Status_t Display_Init(void);

/**
 * @brief Проверить, идёт ли сейчас асинхронная DMA-передача пикселей
 * @return true — драйвер занят, новый вызов Display_WritePixelsDMA/Display_FillColorDMA
 *         вернёт DISPLAY_BUSY; false — можно начинать новую передачу
 */
bool Display_IsBusy(void);

/**
 * @brief Зарегистрировать callback на завершение DMA-передачи
 * @param callback Указатель на функцию, или NULL чтобы отключить уведомление
 */
void Display_RegisterTxCpltCallback(Display_TxCpltCallback_t callback);

/**
 * @brief Задать ориентацию экрана
 * @return DISPLAY_OK при успехе, DISPLAY_BUSY если идёт передача, DISPLAY_ERROR при сбое
 */
Display_Status_t Display_SetRotation(Display_Rotation_t rotation);

/**
 * @brief Получить текущие размеры экрана с учётом ориентации
 */
void Display_GetSize(uint16_t *width, uint16_t *height);

/**
 * @brief Задать прямоугольное окно записи (CASET/RASET у контроллера)
 *
 * Обязательный шаг перед Display_WritePixelsDMA/Display_FillColorDMA.
 * Координаты включительно, в системе текущей ориентации.
 *
 * @return DISPLAY_OK при успехе, DISPLAY_BUSY если идёт передача, DISPLAY_ERROR
 *         при выходе координат за пределы экрана
 */
Display_Status_t Display_SetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief Передать буфер пикселей в ранее установленное окно (асинхронно, через DMA)
 *
 * Не блокирует вызывающий код. Завершение — через Display_TxCpltCallback_t
 * и/или опрос Display_IsBusy(). Буфер pixels должен оставаться валидным до
 * завершения передачи (драйвер его не копирует).
 *
 * @param pixels Буфer RGB565, длиной не менее count элементов
 * @param count  Количество пикселей для передачи
 * @return DISPLAY_OK — передача запущена; DISPLAY_BUSY — предыдущая ещё не завершена
 */
Display_Status_t Display_WritePixelsDMA(const display_color_t *pixels, uint32_t count);

/**
 * @brief Залить ранее установленное окно одним цветом (асинхронно, через DMA)
 *
 * Не требует буфера пикселей в SRAM — источник DMA не инкрементируется.
 *
 * @param color Цвет заливки, RGB565
 * @param count Количество пикселей для заливки (обычно = ширина * высота окна)
 * @return DISPLAY_OK — передача запущена; DISPLAY_BUSY — предыдущая ещё не завершена
 */
Display_Status_t Display_FillColorDMA(display_color_t color, uint32_t count);

/**
 * @brief Перевести контроллер в режим сна (пониженное энергопотребление)
 */
Display_Status_t Display_SleepIn(void);

/**
 * @brief Вывести контроллер из режима сна
 */
Display_Status_t Display_SleepOut(void);

#ifdef __cplusplus
}
#endif

#endif // DISPLAY_H