/**
 * @file diag.c
 * @brief Реализация diag.h — см. правила и обоснования в шапке заголовка.
 */

#include "diag.h"
#include "error.h"
#include "beep.h"
#include "ads1220.h"
#include "settings.h"      /* SETTINGS_TEMP_MAX */
#include "fixed_point.h"
#include "main.h"          /* Solder_Test/Desolder_Test, Solder_On/Desolder_On, nPS_ON */
#include "stm32f4xx_hal.h"

typedef struct {
    GPIO_TypeDef *test_port;
    uint16_t      test_pin;
    GPIO_TypeDef *on_port;
    uint16_t      on_pin;
} diag_pins_t;

static const diag_pins_t s_pins[CHANNEL_COUNT] = {
    [CHANNEL_SOLDER]   = { Solder_Test_GPIO_Port,   Solder_Test_Pin,
                           Solder_On_GPIO_Port,     Solder_On_Pin },
    [CHANNEL_DESOLDER] = { Desolder_Test_GPIO_Port, Desolder_Test_Pin,
                           Desolder_On_GPIO_Port,   Desolder_On_Pin },
};

/** Латч зуммера: один сигнал на ЭПИЗОД аварии, перевзводится при снятии (см. diag.h). */
static bool s_alarm_beeped[CHANNEL_COUNT];

/** Последнее достоверно измеренное состояние нагревателя. */
static bool s_heater_open[CHANNEL_COUNT];

/** Момент Diag_Init() — точка отсчёта грейс-периода на старте АЦП, см. diag.h. */
static uint32_t s_boot_tick;

/** Нагреватель включается НИЗКИМ уровнем -> "неактивен" = высокий. */
static bool heater_drive_inactive(channel_id_t ch)
{
    return HAL_GPIO_ReadPin(s_pins[ch].on_port, s_pins[ch].on_pin) == GPIO_PIN_SET;
}

bool Diag_IsHeaterTestWindowOpen(void)
{
    if (Error_IsPsuFaultActive()) {
        return false;
    }
#if DIAG_GATE_REQUIRE_NPS_ON
    if (HAL_GPIO_ReadPin(nPS_ON_GPIO_Port, nPS_ON_Pin) != GPIO_PIN_RESET) {
        return false; /* nPS_ON активен низким */
    }
#endif
    /* Оба канала должны быть выключены: пока греется хоть один, на общей
     * цепи контроля делать выводы нельзя. */
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (!heater_drive_inactive((channel_id_t)ch)) {
            return false;
        }
    }
    return true;
}

void Diag_Init(void)
{
    s_boot_tick = HAL_GetTick();
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_alarm_beeped[ch] = false;
        /* Фейл-сейф до первого достоверного замера: считаем нагреватель
         * исправным, чтобы не выдать ложную аварию на старте. Реальное
         * состояние приедет в первом же Diag_Poll(), если окно открыто. */
        s_heater_open[ch] = false;
        Error_SetHeaterOpen((channel_id_t)ch, false);
        Error_SetRtdState((channel_id_t)ch, RTD_STATE_OK);
        Error_SetErrorCode((channel_id_t)ch, ERROR_CODE_NONE);
    }
}

/** ERROR_CODE_NONE, либо код неисправности без диагноза, см. diag.h/error.h. */
static error_code_t adc_error_code(channel_id_t ch)
{
    bool no_conversion;

    if (!ADS1220_IsChannelOk(ch)) {
        no_conversion = true; /* SPI-инициализация провалилась */
    } else {
        uint32_t since_ms;
        if (ADS1220_IsDataValid(ch)) {
            since_ms = HAL_GetTick() - ADS1220_GetLastUpdateTick(ch);
        } else {
            since_ms = HAL_GetTick() - s_boot_tick; /* грейс-период на старте */
        }
        no_conversion = since_ms > DIAG_ADC_STALE_MS;
    }
    return no_conversion ? ERROR_CODE_E01_ADC_NO_CONVERSION : ERROR_CODE_NONE;
}

void Diag_Poll(void)
{
    bool window_open = Diag_IsHeaterTestWindowOpen();

    for (int i = 0; i < CHANNEL_COUNT; i++) {
        channel_id_t ch = (channel_id_t)i;

        /* ---- Нагреватель ---- */
        if (window_open) {
            /* Высокий уровень = исправен (см. diag.h) */
            bool ok = HAL_GPIO_ReadPin(s_pins[ch].test_port, s_pins[ch].test_pin) == GPIO_PIN_SET;
            s_heater_open[ch] = !ok;
        }
        /* Публикуем в Error, только когда уже есть хотя бы один валидный
         * отсчёт RTD (тот же гейт, что у блока RTD ниже) — иначе гонка на
         * старте: heater уже измерен (GPIO мгновенно), а RTD ещё держит
         * дефолт RTD_STATE_OK из Diag_Init() (первая конверсия ADS1220
         * придёт позже, ~10-50мс). На этот момент отключенный инструмент
         * читался бы как "нагреватель неисправен + RTD ОК" =
         * TOOL_FAULT_HEATER_OPEN — АВАРИЯ (зуммер!), хотя устоявшееся
         * состояние через 10-50мс — DISCONNECTED (не авария). Латч "один
         * раз за сеанс" в зуммере делает эту гонку не безобидной: сигнал
         * успевает прозвучать до того, как RTD доедет до реального
         * значения, и потом уже не отменится, даже когда экран покажет
         * корректное "--". После первого валидного отсчёта — как раньше,
         * держим последнее достоверное значение, даже если окно теста
         * закрылось. */
        if (ADS1220_IsDataValid(ch)) {
            Error_SetHeaterOpen(ch, s_heater_open[ch]);
        }

        /* ---- Неисправность без диагноза (выше приоритетом, см. error.h) ---- */
        Error_SetErrorCode(ch, adc_error_code(ch));

        /* ---- RTD ---- */
        if (ADS1220_IsDataValid(ch)) {
            fixed_t t = ADS1220_GetTemperatureC(ch);
            rtd_state_t rtd;

            if (t <= 0) {
                rtd = RTD_STATE_SHORT;       /* КЗ: t ~= -301, не ровно 0 — см. diag.h */
            } else if (t > FIXED_FROM_INT(SETTINGS_TEMP_MAX)) {
                rtd = RTD_STATE_OPEN;
            } else {
                rtd = RTD_STATE_OK;
            }
            Error_SetRtdState(ch, rtd);
        }
        /* Конверсий ещё не было — RTD не оцениваем, чтобы стартовый ноль
         * не прочитался как КЗ (см. diag.h). */

        /* ---- Зуммер ---- */
        if (Error_IsChannelAlarm(ch)) {
            if (!s_alarm_beeped[ch]) {
                s_alarm_beeped[ch] = true; /* один раз на ЭПИЗОД аварии — см. чат: пользователь хочет сигнал на
                                             * КАЖДУЮ неисправность, не только на первую за сеанс */
                Beep_Alarm();
            }
        } else {
            /* Авария снята (в т.ч. через DISCONNECTED) — перевзводим: следующее
             * появление аварии на этом канале снова даст сигнал. */
            s_alarm_beeped[ch] = false;
        }
    }
}
