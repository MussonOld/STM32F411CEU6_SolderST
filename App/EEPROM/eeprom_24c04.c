/**
 * @file eeprom_24c04.c
 * @brief Реализация eeprom.h для микросхемы 24C04 (4 Кбит, 512 байт) на I2C1
 *
 * I2C1 (PB6 SCL / PB7 SDA), Fast mode 400 кГц, аппаратные pull-up.
 * Базовый адрес устройства фиксирован (A1..A2 = GND) — см. eeprom_hw.h.
 *
 * Особенность 24C04: 9-битный адрес ячейки (512 байт) при 8-битном слове
 * адреса — старший бит адреса передаётся как бит P0 в самом адресном байте
 * устройства (см. device_addr_for()), а не в байте адреса ячейки.
 *
 * Ожидание готовности после записи страницы реализовано опросом ACK
 * (HAL_I2C_IsDeviceReady) вместо фиксированной задержки — это быстрее
 * в среднем и не зависит от точного времени tWR конкретного экземпляра
 * микросхемы. Опрос ограничен по времени через HAL_GetTick(), без
 * блокирующего HAL_Delay внутри цикла ожидания.
 */

#include "eeprom.h"
#include "eeprom_hw.h"
#include "stm32f4xx_hal.h"

extern I2C_HandleTypeDef hi2c1;

#define EEPROM_I2C_TIMEOUT_MS  (50U)

/* ---- Внутренние примитивы ---- */

/**
 * @brief Собрать адресный байт устройства с учётом бита выбора блока (P0)
 * @param addr Полный (9-битный) адрес ячейки, 0 .. EEPROM_HW_SIZE_BYTES-1
 */
static inline uint8_t device_addr_for(uint16_t addr)
{
    uint8_t block = (uint8_t)((addr >> 8) & 0x01);
    return (uint8_t)(EEPROM_HW_I2C_ADDR_BASE | (block << EEPROM_HW_BLOCK_BIT_POS));
}

/**
 * @brief Дождаться завершения внутреннего цикла записи микросхемы
 * @return EEPROM_OK, если микросхема ответила ACK на своё адресное слово;
 *         EEPROM_TIMEOUT, если не уложились в EEPROM_HW_WRITE_TIMEOUT_MS
 */
static EEPROM_Status_t wait_write_complete(uint8_t device_addr)
{
    uint32_t start = HAL_GetTick();

    while ((HAL_GetTick() - start) < EEPROM_HW_WRITE_TIMEOUT_MS) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, device_addr,
                                   EEPROM_HW_READY_TRIALS, 1) == HAL_OK) {
            return EEPROM_OK;
        }
    }
    return EEPROM_TIMEOUT;
}

/**
 * @brief Записать один чанк, не пересекающий границу страницы и не
 *        пересекающий границу блока (addr и addr+len-1 — в одном блоке)
 */
static EEPROM_Status_t write_chunk(uint16_t addr, const uint8_t *data, uint16_t len)
{
    uint8_t device_addr = device_addr_for(addr);
    uint8_t word_addr    = (uint8_t)(addr & 0xFF);

    HAL_StatusTypeDef hal_status = HAL_I2C_Mem_Write(
        &hi2c1, device_addr, word_addr, I2C_MEMADD_SIZE_8BIT,
        (uint8_t *)data, len, EEPROM_I2C_TIMEOUT_MS);

    if (hal_status != HAL_OK) {
        return EEPROM_ERROR;
    }

    return wait_write_complete(device_addr);
}

/**
 * @brief Прочитать один чанк, не пересекающий границу блока
 */
static EEPROM_Status_t read_chunk(uint16_t addr, uint8_t *data, uint16_t len)
{
    uint8_t device_addr = device_addr_for(addr);
    uint8_t word_addr    = (uint8_t)(addr & 0xFF);

    HAL_StatusTypeDef hal_status = HAL_I2C_Mem_Read(
        &hi2c1, device_addr, word_addr, I2C_MEMADD_SIZE_8BIT,
        data, len, EEPROM_I2C_TIMEOUT_MS);

    return (hal_status == HAL_OK) ? EEPROM_OK : EEPROM_ERROR;
}

/* ---- Реализация контракта eeprom.h ---- */

EEPROM_Status_t EEPROM_Init(void)
{
    if (HAL_I2C_IsDeviceReady(&hi2c1, EEPROM_HW_I2C_ADDR_BASE,
                               2, EEPROM_I2C_TIMEOUT_MS) != HAL_OK) {
        return EEPROM_TIMEOUT;
    }
    return EEPROM_OK;
}

uint16_t EEPROM_GetSize(void)
{
    return EEPROM_HW_SIZE_BYTES;
}

EEPROM_Status_t EEPROM_WriteByte(uint16_t addr, uint8_t data)
{
    if (addr >= EEPROM_HW_SIZE_BYTES) {
        return EEPROM_ERROR;
    }
    return write_chunk(addr, &data, 1);
}

EEPROM_Status_t EEPROM_ReadByte(uint16_t addr, uint8_t *data)
{
    if (addr >= EEPROM_HW_SIZE_BYTES || data == NULL) {
        return EEPROM_ERROR;
    }
    return read_chunk(addr, data, 1);
}

EEPROM_Status_t EEPROM_Write(uint16_t addr, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return EEPROM_ERROR;
    }
    if ((uint32_t)addr + len > EEPROM_HW_SIZE_BYTES) {
        return EEPROM_ERROR;
    }

    uint16_t written = 0;

    while (written < len) {
        uint16_t cur_addr = addr + written;
        uint16_t remaining = len - written;

        /* Сколько байт осталось до конца текущей страницы */
        uint16_t space_in_page = EEPROM_HW_PAGE_SIZE -
                                  (cur_addr % EEPROM_HW_PAGE_SIZE);
        uint16_t chunk = (remaining < space_in_page) ? remaining : space_in_page;

        /* Страница физически не пересекает границу блока (256 байт кратно
         * 16), так что дополнительная защита от пересечения блока внутри
         * одного чанка не требуется. */
        EEPROM_Status_t status = write_chunk(cur_addr, data + written, chunk);
        if (status != EEPROM_OK) {
            return status;
        }

        written += chunk;
    }

    return EEPROM_OK;
}

EEPROM_Status_t EEPROM_Read(uint16_t addr, uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return EEPROM_ERROR;
    }
    if ((uint32_t)addr + len > EEPROM_HW_SIZE_BYTES) {
        return EEPROM_ERROR;
    }

    uint16_t read_total = 0;

    while (read_total < len) {
        uint16_t cur_addr = addr + read_total;
        uint16_t remaining = len - read_total;

        /* HAL_I2C_Mem_Read с 8-битным адресом ячейки сам инкрементирует
         * внутренний указатель микросхемы, но не может перейти через
         * границу блока (девятый бит адреса зашит в адресный байт
         * устройства и не меняется автоматически) — поэтому чтение,
         * как и запись, режем по границе блока (256 байт). */
        uint16_t space_in_block = 256U - (cur_addr % 256U);
        uint16_t chunk = (remaining < space_in_block) ? remaining : space_in_block;

        EEPROM_Status_t status = read_chunk(cur_addr, data + read_total, chunk);
        if (status != EEPROM_OK) {
            return status;
        }

        read_total += chunk;
    }

    return EEPROM_OK;
}
