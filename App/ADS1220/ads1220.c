/**
 * @file ads1220.c
 * @brief Реализация драйвера ADS1220 — см. ads1220.h для схемы включения,
 *        вывода формул и конфигурации регистров.
 */

#include "ads1220.h"
#include "spi.h"   /* hspi2 */
#include "main.h"  /* ADS1220_Solder_CS_*, ADS1220_Desolder_CS_*, DRDY_Solder_*, DRDY_Desolder_* */

/* ---- Команды ADS1220 (datasheet, Table "Command Definitions") ---- */
#define ADS1220_CMD_RESET      0x06U
#define ADS1220_CMD_START_SYNC 0x08U
#define ADS1220_CMD_POWERDOWN  0x02U
#define ADS1220_CMD_RDATA      0x10U
#define ADS1220_CMD_WREG_BASE  0x40U /* | (start_reg << 2) | (n - 1) */

/* ---- Байты конфигурации — см. вывод в ads1220.h ---- */
#define ADS1220_CFG0 0x66U /* MUX=0110, GAIN=011(x8), PGA_BYPASS=0 */
#define ADS1220_CFG1 0x04U /* DR=000(20SPS), MODE=00, CM=1(continuous) */
#define ADS1220_CFG2 0x76U /* VREF=01(REFP0/REFN0), 50/60=11, IDAC=110(1000мкА) */
#define ADS1220_CFG3 0x80U /* I1MUX=100(AIN3/REFN1), I2MUX=000 */

#define ADS1220_FULL_SCALE_CODE 8388608 /* 2^23 */

/* Таймауты SPI-транзакций (диагностика — см. чат/память проекта по
 * поводу того, почему это ВАЖНО проверять, а не игнорировать молча). */
#define ADS1220_SPI_TIMEOUT_MS 50U

typedef struct {
    GPIO_TypeDef *cs_port;
    uint16_t      cs_pin;
    GPIO_TypeDef *drdy_port;
    uint16_t      drdy_pin;
} ads1220_pins_t;

static const ads1220_pins_t s_pins[CHANNEL_COUNT] = {
    [CHANNEL_SOLDER]   = { ADS1220_Solder_CS_GPIO_Port,   ADS1220_Solder_CS_Pin,
                            DRDY_Solder_GPIO_Port,         DRDY_Solder_Pin },
    [CHANNEL_DESOLDER] = { ADS1220_Desolder_CS_GPIO_Port, ADS1220_Desolder_CS_Pin,
                            DRDY_Desolder_GPIO_Port,       DRDY_Desolder_Pin },
};

typedef struct {
    bool    data_valid;
    bool    init_ok;
    int32_t raw_code;
} ads1220_state_t;

static ads1220_state_t s_state[CHANNEL_COUNT];

static void cs_select(channel_id_t ch)
{
    HAL_GPIO_WritePin(s_pins[ch].cs_port, s_pins[ch].cs_pin, GPIO_PIN_RESET);
}

static void cs_deselect(channel_id_t ch)
{
    HAL_GPIO_WritePin(s_pins[ch].cs_port, s_pins[ch].cs_pin, GPIO_PIN_SET);
}

static bool data_ready(channel_id_t ch)
{
    /* DRDY активен низким уровнем */
    return HAL_GPIO_ReadPin(s_pins[ch].drdy_port, s_pins[ch].drdy_pin) == GPIO_PIN_RESET;
}

/** Простая команда без данных (RESET/START-SYNC/POWERDOWN). CS должен
 *  быть уже выставлен вызывающим кодом (или выставляется/снимается
 *  здесь для одиночных команд — см. использование ниже). */
static bool send_command(channel_id_t ch, uint8_t cmd)
{
    cs_select(ch);
    bool ok = (HAL_SPI_Transmit(&hspi2, &cmd, 1, ADS1220_SPI_TIMEOUT_MS) == HAL_OK);
    cs_deselect(ch);
    return ok;
}

/** WREG нескольких регистров подряд, начиная с start_reg. */
static bool write_regs(channel_id_t ch, uint8_t start_reg, const uint8_t *values, uint8_t n)
{
    uint8_t cmd = (uint8_t)(ADS1220_CMD_WREG_BASE | (start_reg << 2) | (n - 1U));

    cs_select(ch);
    bool ok = (HAL_SPI_Transmit(&hspi2, &cmd, 1, ADS1220_SPI_TIMEOUT_MS) == HAL_OK);
    if (ok) {
        ok = (HAL_SPI_Transmit(&hspi2, (uint8_t *)values, n, ADS1220_SPI_TIMEOUT_MS) == HAL_OK);
    }
    cs_deselect(ch);
    return ok;
}

/** RDATA — читает готовый 24-битный отсчёт. Возвращает false при ошибке
 *  SPI (см. чат про важность НЕ игнорировать молча возврат
 *  HAL_SPI_Transmit/Receive). */
static bool read_data(channel_id_t ch, int32_t *out_code)
{
    uint8_t cmd = ADS1220_CMD_RDATA;
    uint8_t rx[3] = { 0 };
    uint8_t tx_dummy[3] = { 0x00, 0x00, 0x00 };

    cs_select(ch);
    bool ok = (HAL_SPI_Transmit(&hspi2, &cmd, 1, ADS1220_SPI_TIMEOUT_MS) == HAL_OK);
    if (ok) {
        ok = (HAL_SPI_TransmitReceive(&hspi2, tx_dummy, rx, 3, ADS1220_SPI_TIMEOUT_MS) == HAL_OK);
    }
    cs_deselect(ch);

    if (!ok) {
        return false;
    }

    int32_t code = ((int32_t)rx[0] << 16) | ((int32_t)rx[1] << 8) | (int32_t)rx[2];
    if (code & 0x00800000) {
        code |= (int32_t)0xFF000000; /* знаковое расширение 24 -> 32 бит */
    }
    *out_code = code;
    return true;
}

static bool init_channel(channel_id_t ch)
{
    uint8_t cfg[4] = { ADS1220_CFG0, ADS1220_CFG1, ADS1220_CFG2, ADS1220_CFG3 };
    bool ok;

    cs_deselect(ch);

    /* RESET (сериальная команда — у ADS1220 нет отдельного HW RESET-пина).
     * После неё нужна пауза перед следующей транзакцией (датащит: не
     * менее ~0.6 мс; берём с запасом). */
    ok = send_command(ch, ADS1220_CMD_RESET);
    HAL_Delay(2);

    ok = write_regs(ch, 0, cfg, 4) && ok;

    /* Запуск continuous conversion. */
    ok = send_command(ch, ADS1220_CMD_START_SYNC) && ok;

    return ok;
}

void ADS1220_Init(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_state[ch].data_valid = false;
        s_state[ch].raw_code   = 0;
        s_state[ch].init_ok    = init_channel((channel_id_t)ch);
    }
}

void ADS1220_Poll(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (!s_state[ch].init_ok || !data_ready((channel_id_t)ch)) {
            continue;
        }
        int32_t code;
        if (read_data((channel_id_t)ch, &code)) {
            s_state[ch].raw_code   = code;
            s_state[ch].data_valid = true;
        }
        /* При ok==false просто пропускаем этот цикл — следующий DRDY
         * придёт своим чередом (continuous mode), данные не теряются
         * систематически, только этот единичный отсчёт. */
    }
}

bool ADS1220_IsDataValid(channel_id_t ch)
{
    if (!channel_valid(ch)) {
        return false;
    }
    return s_state[ch].data_valid;
}

bool ADS1220_IsChannelOk(channel_id_t ch)
{
    if (!channel_valid(ch)) {
        return false;
    }
    return s_state[ch].init_ok;
}

int32_t ADS1220_GetRawCode(channel_id_t ch)
{
    if (!channel_valid(ch)) {
        return 0;
    }
    return s_state[ch].raw_code;
}

fixed_t ADS1220_GetResistanceOhm(channel_id_t ch)
{
    if (!channel_valid(ch)) {
        return 0;
    }
    /* R = code/2^23 * Rref / gain, в Q16.16:
     *   R_fixed = code * Rref * FIXED_ONE / (2^23 * gain) */
    int64_t num = (int64_t)s_state[ch].raw_code * ADS1220_RREF_OHM * FIXED_ONE;
    int64_t den = (int64_t)ADS1220_FULL_SCALE_CODE * ADS1220_GAIN;
    return (fixed_t)(num / den);
}

fixed_t ADS1220_GetTemperatureC(channel_id_t ch)
{
    /* t[°C] = (R - 21.7) / 0.072 — паспортная формула RTD, см. чат. */
    static const fixed_t offset_ohm     = FIXED_FROM_FLOAT(21.7f);
    static const fixed_t slope_ohm_degc = FIXED_FROM_FLOAT(0.072f);

    fixed_t r = ADS1220_GetResistanceOhm(ch);
    return fixed_div(r - offset_ohm, slope_ohm_degc);
}
