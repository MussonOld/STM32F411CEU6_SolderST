/**
 * @file sleep.h
 * @brief Таймеры простоя (Presleep/Sleep) по инструментам.
 *
 * Модель — два НЕЗАВИСИМЫХ порога на канал, отсчитываемых от одного и
 * того же момента начала простоя:
 *  - PreSleepTimeout: по истечении Settings_GetPreSleepTimeout() минут
 *    простоя канал переходит в PRESLEEP.
 *  - SleepTimeout: по истечении Settings_GetSleepTimeout() минут простоя
 *    канал переходит в SLEEP — НЕЗАВИСИМО от того, сработал ли PRESLEEP.
 *    Если SleepTimeout наступает раньше PreSleepTimeout (или
 *    PreSleepTimeout==0, т.е. выключен), канал уходит из AWAKE сразу в
 *    SLEEP, минуя PRESLEEP.
 *
 * PRESLEEP: рабочая уставка (State setpoint) НЕ меняется, но эффективная
 * температура нагрева должна браться из Settings_GetPresleepTemp() — это
 * решение уровня Control (PID), Sleep сам ничего не пишет ни в State, ни
 * в нагреватель. SLEEP: нагрев инструмента полностью отключается (не
 * путать с ручным отключением канала аккордом UP+DN, см. fsm.h) — тоже
 * решение уровня Control, основанное на Sleep_GetMode().
 *
 * Источник "простаивает" — РАЗНЫЙ на канал (не унифицирован специально,
 * так задано в спецификации проекта):
 *  - CHANNEL_SOLDER:   Dock_Pin (датчик подставки). Простой = паяльник
 *                       запаркован (Dock==1, активный уровень ВЫСОКИЙ —
 *                       проверено на железе, не совпадает с Btn_Pump ниже;
 *                       GPIO_PULLUP — см. gpio.c). Опрашивается поллингом
 *                       здесь же; аппаратный EXTI на этом пине (см. .ioc/
 *                       stm32f4xx_it.c) сейчас ничего не делает и этому
 *                       модулю не мешает.
 *  - CHANNEL_DESOLDER: Btn_Pump_Pin (кнопка помпы). Простой = кнопка
 *                       отпущена (активный уровень низкий, GPIO_PULLUP).
 *
 * Оба входа НЕ входят в Buttons (buttons.h) — это отдельные сырые GPIO,
 * дебаунсятся здесь же по тому же принципу (N стабильных опросов подряд),
 * что и в buttons.c.
 *
 * Sleep_Poll() — вызывать из главного цикла с периодом ~SLEEP_POLL_MS
 * (как и Buttons_Poll()). Не блокирует.
 *
 * Переход простой -> занят (в любой момент, из любого режима) — единый
 * таймер простоя останавливается и сбрасывается, канал сразу
 * возвращается в AWAKE. Если PreSleepTimeout или SleepTimeout настроены
 * в 0 — соответствующий переход не происходит вообще (0 минут = функция
 * выключена, тот же принцип уже используется в Settings_Init()); при
 * SleepTimeout==0 канал может зависнуть в PRESLEEP бессрочно.
 */

#ifndef SLEEP_H
#define SLEEP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "channel.h" /* channel_id_t — общий тип, см. App/Common/channel.h */

/* ---- Тайминги поллинга/дебаунса (тот же принцип, что в buttons.h) ---- */
#define SLEEP_POLL_MS          10U  /* ожидаемый период вызова Sleep_Poll() */
#define SLEEP_DEBOUNCE_TICKS    3U  /* подряд стабильных опросов для подтверждения уровня */

/**
 * @brief Режим простоя канала
 */
typedef enum {
    SLEEP_MODE_AWAKE = 0,  /**< Инструмент используется (или простаивает менее обоих порогов) */
    SLEEP_MODE_PRESLEEP,   /**< PreSleepTimeout сработал: держать Settings_GetPresleepTemp() */
    SLEEP_MODE_SLEEP       /**< SleepTimeout сработал: нагрев инструмента отключён */
} sleep_mode_t;

/**
 * @brief Инициализация: все каналы AWAKE, таймеры остановлены/сброшены
 */
void Sleep_Init(void);

/**
 * @brief Один шаг опроса — вызывать из главного цикла с периодом ~SLEEP_POLL_MS
 */
void Sleep_Poll(void);

/** @brief Текущий режим простоя канала */
sleep_mode_t Sleep_GetMode(channel_id_t ch);

/**
 * @brief Сколько секунд осталось до СЛЕДУЮЩЕГО перехода режима
 *
 * AWAKE  — до ближайшего из двух порогов (PreSleepTimeout/SleepTimeout),
 *          какой наступит раньше и включён; 0, если инструмент не
 *          простаивает ИЛИ оба порога выключены (0 минут, см. Settings).
 * PRESLEEP — до SLEEP (порог SleepTimeout); 0, если SleepTimeout
 *          выключен — канал останется в PRESLEEP бессрочно, дальше не
 *          считаем.
 * SLEEP  — уже финальный режим, дальше считать нечего, всегда 0.
 */
uint32_t Sleep_GetRemainingSeconds(channel_id_t ch);

#ifdef __cplusplus
}
#endif

#endif /* SLEEP_H */
