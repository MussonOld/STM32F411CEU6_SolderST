/**
 * @file fonts.h
 * @brief Универсальная структура для пропорциональных шрифтов
 *
 * Поддерживает шрифты во внутренней памяти (ROM/Flash) и внешней Flash (25Qxx)
 */

#ifndef FONTS_H
#define FONTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @brief Тип хранения шрифта
 */
typedef enum {
	FONT_STORAGE_INTERNAL, /**< Шрифт во внутренней памяти (ROM/Flash MCU) */
	FONT_STORAGE_EXTERNAL /**< Шрифт во внешней Flash памяти (например, 25Qxx) */
} font_storage_t;

/**
 * @brief Структура пропорционального шрифта
 *
 * Содержит всю необходимую информацию для отрисовки шрифта с переменной шириной символов
 */
typedef struct {
	const uint8_t *bitmap; /**< Пиксельные данные всех символов */
	const uint8_t *widths; /**< Ширина bounding box каждого символа */
	const uint16_t *offsets; /**< Смещение в bitmap для каждого символа */
	const int8_t *xoffset; /**< Горизонтальный сдвиг символа (может быть отрицательным) */
	const int8_t *yoffset; /**< Вертикальный сдвиг от baseline (может быть отрицательным) */
	const uint8_t *dwidth; /**< Шаг курсора после символа (device width / advance) */
	const uint8_t *glyph_heights; /**< Реальная высота каждого символа в пикселях */

	const uint16_t *lut;       // Таблица Unicode кодов
	uint16_t lut_size;         // Количество символов
	uint8_t height; /**< Общая высота рамки шрифта (font bounding box height) */
	uint16_t first_char; /**< Первый символ в шрифте (обычно 32 = пробел) */
	uint16_t last_char; /**< Последний символ в шрифте (обычно 126 = ~) */

	font_storage_t storage_type; /**< Тип хранения шрифта (внутренняя/внешняя память) */
	uint32_t flash_address; /**< Базовый адрес во внешней Flash (используется только для FONT_STORAGE_EXTERNAL) */
} font_t;

// Объявления шрифтов
extern const font_t AntiquaB_16_uni;
extern const font_t AntiquaB_18_uni;
extern const font_t AntiquaB_24_uni;
extern const font_t Comic_36_dig;
extern const font_t Comic_40_dig;
extern const font_t Comic_60_dig;

#ifdef __cplusplus
}
#endif

#endif // FONTS_H
