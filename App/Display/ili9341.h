/**
 * @file ili9341.h
 * @brief Приватные константы контроллера ILI9341 (команды, MADCTL, init-последовательность, тайминги)
 *
 * Не инклюдится ничем, кроме ili9341.c. Верхний слой работает только через display.h.
 */

#ifndef ILI9341_H
#define ILI9341_H

/* Физическое разрешение RAM контроллера (портретная ориентация "как есть") —
 * те же 240x320, что были прописаны и для ST7789 в этом проекте (см.
 * st7789.h) — физическая панель, судя по всему, изначально была ILI9341-класса. */
#define ILI9341_RAM_WIDTH   240
#define ILI9341_RAM_HEIGHT  320

/* Команды общие с ST7789 (Datasheet ILI9341, раздел "Command list") —
 * именно поэтому на ST7789-драйвере панель "иногда запускается": базовые
 * команды (CASET/RASET/RAMWR/MADCTL/COLMOD/SLPOUT/...) действительно
 * совпадают, но у ILI9341 после SWRESET/SLPOUT включены не все внутренние
 * узлы (питание панели, VCOM, гамма-коррекция) — без явной настройки
 * Power Control/VCOM Control контроллер иногда остаётся с "плывущими"
 * дефолтами: может изредка стартовать более-менее нормально, но нестабильно. */
#define ILI9341_CMD_SWRESET  0x01
#define ILI9341_CMD_SLPIN    0x10
#define ILI9341_CMD_SLPOUT   0x11
#define ILI9341_CMD_INVOFF   0x20
#define ILI9341_CMD_INVON    0x21
#define ILI9341_CMD_DISPOFF  0x28
#define ILI9341_CMD_DISPON   0x29
#define ILI9341_CMD_CASET    0x2A
#define ILI9341_CMD_RASET    0x2B
#define ILI9341_CMD_RAMWR    0x2C
#define ILI9341_CMD_NORON    0x13
#define ILI9341_CMD_MADCTL   0x36
#define ILI9341_CMD_COLMOD   0x3A

/* ---- Команды, специфичные для ILI9341 (у ST7789 отсутствуют/другие адреса) ---- */
#define ILI9341_CMD_PWCTRB       0xCF /* Power Control B */
#define ILI9341_CMD_POSC         0xED /* Power On Sequence Control */
#define ILI9341_CMD_DRVTIMA      0xE8 /* Driver Timing Control A */
#define ILI9341_CMD_PWSEQCTRL    0xF7 /* Pump Ratio Control */
#define ILI9341_CMD_DRVTIMB      0xEA /* Driver Timing Control B */
#define ILI9341_CMD_PWCTR1       0xC0 /* Power Control 1 */
#define ILI9341_CMD_PWCTR2       0xC1 /* Power Control 2 */
#define ILI9341_CMD_VMCTR1       0xC5 /* VCOM Control 1 */
#define ILI9341_CMD_VMCTR2       0xC7 /* VCOM Control 2 */
#define ILI9341_CMD_FRMCTR1      0xB1 /* Frame Rate Control (normal mode) */
#define ILI9341_CMD_DFUNCTR      0xB6 /* Display Function Control */
#define ILI9341_CMD_EN3GAM       0xF2 /* Enable 3G (gamma) */
#define ILI9341_CMD_GAMSET       0x26 /* Gamma Set */
#define ILI9341_CMD_GMCTRP1      0xE0 /* Positive Gamma Correction */
#define ILI9341_CMD_GMCTRN1      0xE1 /* Negative Gamma Correction */

/* ВРЕМЕННО (см. чат): команды чтения статусных регистров для диагностики
 * через MISO (PA6, временно переброшен с DRDY_Desolder) — сравнить
 * RDDPM/RDDSDR на первом (сейчас безусловно самосбрасываемом) и втором
 * проходе double-pass, чтобы понять, готов ли контроллер физически к
 * моменту команд на первом проходе, или дело в чём-то другом. */
#define ILI9341_CMD_RDDPM        0x0A /* Read Display Power Mode */
#define ILI9341_CMD_RDDSDR       0x0F /* Read Display Self-Diagnostic Result */

/* MADCTL биты (Memory Access Control) — расположение как у ST7789, плюс BGR */
#define ILI9341_MADCTL_MY    0x80  /* Row address order    */
#define ILI9341_MADCTL_MX    0x40  /* Column address order  */
#define ILI9341_MADCTL_MV    0x20  /* Row/Column exchange   */
#define ILI9341_MADCTL_RGB   0x00  /* Порядок цвета RGB (не BGR) — НЕ подошёл этой панели, см. BGR ниже */
#define ILI9341_MADCTL_BGR   0x08  /* Порядок цвета BGR — подтверждено на
                                     * этой панели (жёлтый выходил голубым на
                                     * RGB — классический признак перепутанных
                                     * R/B, см. Display_SetRotation() в
                                     * ili9341.c, сейчас используется этот бит) */

/* COLMOD: 0x55 = 16 бит/пиксель (RGB565) */
#define ILI9341_COLMOD_16BPP 0x55

/* Тайминги (datasheet, мс) — тот же порядок величин, что и у ST7789 */
#define ILI9341_DELAY_RESET_MS     10  /* Длительность низкого уровня RESX */
#define ILI9341_DELAY_AFTER_RST_MS 120 /* Пауза после аппаратного reset    */
#define ILI9341_DELAY_SLPOUT_MS    120 /* Пауза после Sleep Out            */

#endif /* ILI9341_H */
