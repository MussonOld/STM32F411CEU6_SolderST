/**
 * @file ads1220.h
 * @brief Драйвер ADS1220 (24-бит delta-sigma АЦП) для измерения температуры
 *        платинового RTD жала, по каналу на Solder/Desolder.
 *
 * Схема (см. чат/схематик, U7 на канале Solder, зеркально на Desolder):
 *
 *   AIN3/REFN1 (IDAC1, 1000мкА) -> SRTD1 -> [внешний RTD] -> SRTD2 ->
 *      -> Rref1 (1кОм, на плате) -> AGND
 *
 *   Kelvin-сенсинг (через фильтрующие R37-R40, 100 Ом — тока в АЦП-входы
 *   практически нет, IR-падение на них пренебрежимо):
 *     - падение на RTD:  AIN1(+), AIN0(-)
 *     - падение на Rref: REFP0(+), REFN0(-)  — одновременно СЛУЖИТ
 *       опорным напряжением АЦП (ratiometric-измерение: ток IDAC входит и
 *       в сигнал, и в опору, поэтому его абсолютная точность/дрейф не
 *       влияют на результат — важна только точность Rref).
 *
 *   Rref1: Yageo RT0603BRD071KL — тонкоплёночный прецизионный резистор,
 *   1кОм ±0.1%, ТКС ±25ppm/°C (0603). Вся точность измерения R_rtd
 *   напрямую наследуется от точности этого резистора (см. выше).
 *
 * Из этого: R_rtd = code/2^23 * Rref / gain (см. ADS1220_RREF_OHM/
 * ADS1220_GAIN ниже) — вывод формулы и выбор gain=8 (вместо более точного
 * по шуму, но клиппингующего с меньшим запасом gain=16) см. в чате.
 *
 * Конфигурация регистров (см. datasheet SBAS501/ADS1220):
 *   Config0 = MUX=0110(AIN1+/AIN0-), GAIN=011(x8), PGA_BYPASS=0
 *   Config1 = DR=000(20SPS), MODE=00(Normal), CM=1(continuous), TS=0, BCS=0
 *   Config2 = VREF=01(REFP0/REFN0), 50/60=11(одновременное подавление
 *             50/60Гц — датащит явно требует ЭТИ биты, а не только низкий
 *             DR, для одновременного подавления обеих частот), PSW=0,
 *             IDAC=110(1000мкА)
 *   Config3 = I1MUX=100(AIN3/REFN1), I2MUX=000(выкл), DRDYM=0
 *
 * RTD нестандартный, формула по паспорту сенсора (см. чат):
 *   t[°C] = (R[Ом] - bias) / slope
 * номиналы bias=21.7 Ом, slope=0.072 Ом/°C, но реально берутся из Settings
 * (Settings_GetBias/GetSlope, меню Expert) — калибровка канала.
 *
 * Использование:
 *   MX_SPI2_Init() должен быть вызван ДО ADS1220_Init() (см. main.c).
 *   ADS1220_Init()  — один раз при старте, конфигурирует оба канала.
 *   ADS1220_Poll()  — из главного цикла, период ~ADS1220_POLL_MS.
 *   ADS1220_GetTemperatureC(ch) / ADS1220_GetResistanceOhm(ch) — геттеры
 *   последнего валидного отсчёта, не блокируют.
 */

#ifndef ADS1220_H
#define ADS1220_H

#include <stdint.h>
#include <stdbool.h>
#include "channel.h"     /* channel_id_t — App/Common/channel.h */
#include "fixed_point.h" /* fixed_t (Q16.16) — App/Common/fixed_point.h */

#ifdef __cplusplus
extern "C" {
#endif

/** Период вызова ADS1220_Poll() из главного цикла. DRDY при 20SPS
 *  обновляется раз в ~50 мс — опрашивать чаще дёшево (это просто чтение
 *  GPIO), поэтому берём тот же порядок, что и у остальных Poll() в
 *  проекте (Buttons/Sleep), а не подстраиваемся под 50 мс отдельно. */
#define ADS1220_POLL_MS 10U

/** Параметры измерительной цепи — см. комментарий выше и чат по выбору. */
#define ADS1220_RREF_OHM      1000
#define ADS1220_GAIN          8
#define ADS1220_IDAC_MICROAMP 1000

/**
 * @brief Инициализация: сброс и конфигурация обоих каналов (Solder/
 *        Desolder) через SPI2, запуск continuous conversion.
 * @note  MX_SPI2_Init() должен быть вызван раньше.
 */
void ADS1220_Init(void);

/**
 * @brief Неблокирующий опрос — вызывать из главного цикла с периодом
 *        ~ADS1220_POLL_MS. Проверяет DRDY каждого канала; если данные
 *        готовы — вычитывает результат (RDATA) и кэширует его для
 *        последующих Get*-запросов.
 */
void ADS1220_Poll(void);

/** @brief Был ли хотя бы один успешный отсчёт с момента ADS1220_Init(). */
bool ADS1220_IsDataValid(channel_id_t ch);

/** @brief Прошла ли SPI-инициализация канала без ошибок (RESET/WREG/START —
 *         см. чат про важность НЕ игнорировать молча возврат HAL_SPI_*).
 *         false — данные с канала недостоверны в принципе, не только
 *         "ещё не готовы" (в отличие от ADS1220_IsDataValid()). */
bool ADS1220_IsChannelOk(channel_id_t ch);

/** @brief Сырой 24-битный код АЦП, знаково расширенный до int32_t,
 *         последнее считанное значение (для отладки/диагностики). */
int32_t ADS1220_GetRawCode(channel_id_t ch);

/**
 * @brief HAL_GetTick() момента последней УСПЕШНОЙ конверсии (RDATA), или 0,
 *        если их ещё не было. Даёт отличить "канал живой, просто редкий
 *        сбой SPI" от "перестал отвечать вообще" — см. Diag (diag.c),
 *        который сравнивает это со свежим HAL_GetTick() против таймаута.
 */
uint32_t ADS1220_GetLastUpdateTick(channel_id_t ch);

/** @brief Сопротивление RTD, Ом, Q16.16. */
fixed_t ADS1220_GetResistanceOhm(channel_id_t ch);

/** @brief Температура жала, °C, Q16.16. */
fixed_t ADS1220_GetTemperatureC(channel_id_t ch);

#ifdef __cplusplus
}
#endif

#endif /* ADS1220_H */
