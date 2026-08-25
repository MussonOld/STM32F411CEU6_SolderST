/**
 * @file settings.h
 * @brief Конфигурация станции: пресеты, текущая уставка, sleep-параметры,
 *        калибровка (slope/bias), тайминги sleep, коэффициенты PID и
 *        глобальные флаги. Персистится в EEPROM целиком, с отложенной
 *        записью через Settings_Poll().
 *
 * Чистое хранилище данных — намеренно не знает ни про state.h (fixed_t/PID),
 * ни про buttons.h. Разделение ответственности то же, что уже закреплено в
 * state.h: "Ввод (кнопки) пишет setpoint_temp" — этот же слой (Input/UI),
 * выбирая пресет или меняя target, обязан сам продублировать значение в
 * State через State_SetSetpointTemp(ch, FIXED_FROM_INT(value)). Settings
 * не должен дёргать State самостоятельно — иначе в setpoint_temp появятся
 * два независимых писателя.
 *
 * Kp/Ki/Kd тоже uint16_t (сырое хранимое значение) — синхронизировать не с
 * чем: зеркального поля в state.h нет. Control читает их напрямую
 * (Settings_GetKp/Ki/Kd) и сам конвертирует в fixed_t при необходимости —
 * межслойное чтение уже допустимо по тому же прецеденту, что и чтение
 * setpoint_temp Control-ом из State.
 *
 * uint16_t вместо fixed_t — здесь удобно для UI (отображение целых
 * градусов/величин) и для записи в EEPROM (2 байта на значение).
 *
 * ВСЕ сеттеры клампят входное значение к допустимому диапазону (см. ниже) —
 * значение молча зажимается к границе, не отбрасывается.
 *
 * ПЕРСИСТЕНЦИЯ: любой сеттер, реально изменивший значение (после клампинга),
 * взводит внутренний таймер отложенной записи на SETTINGS_SAVE_DELAY_MS.
 * Settings_Poll() — вызывать периодически из главного цикла — фактически
 * пишет в EEPROM, когда с последнего изменения прошло SETTINGS_SAVE_DELAY_MS
 * без новых изменений ("отпустили кнопку и подождали"). Сама Settings_Save()
 * тоже доступна публично для принудительного немедленного сохранения.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "channel.h" /* channel_id_t — общий тип, см. App/Common/channel.h */

/* ---- Задержка отложенной записи в EEPROM ---- */
#define SETTINGS_SAVE_DELAY_MS  (1500U)

/* ---- Диапазоны значений (все сеттеры клампят к этим границам) ---- */

/** @brief Температурные уставки (preset*, target, presleepTemp), °C */
#define SETTINGS_TEMP_MIN  (50U)
#define SETTINGS_TEMP_MAX  (450U)

/** @brief Тайминги Sleep, минуты */
#define SETTINGS_SLEEP_TIMEOUT_MIN_MINUTES  (0U)
#define SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES  (30U)

/**
 * @brief Масштаб хранения Slope: реальное значение = Settings_GetSlope(ch) / SETTINGS_SLOPE_SCALE
 * @note Диапазон ВРЕМЕННЫЙ (назначен без уточнения от пользователя, см. чат) —
 *       уточнить позже. Нижняя граница обязана быть > 0: slope используется
 *       как делитель в формуле t=(R-bias)/slope, ноль недопустим.
 */
#define SETTINGS_SLOPE_SCALE  (1000U)
#define SETTINGS_SLOPE_MIN    (1U)      /* реальное 0.001 — минимум, чтобы не было деления на 0 */
#define SETTINGS_SLOPE_MAX    (10000U)  /* реальное 10.0 — ВРЕМЕННО, уточнить */

/**
 * @brief Масштаб хранения Bias: реальное значение = Settings_GetBias(ch) / SETTINGS_BIAS_SCALE
 * @note Диапазон ВРЕМЕННЫЙ, уточнить позже.
 */
#define SETTINGS_BIAS_SCALE  (10U)
#define SETTINGS_BIAS_MIN    (0U)
#define SETTINGS_BIAS_MAX    (1000U)  /* реальное 100.0 — ВРЕМЕННО, уточнить */

/**
 * @brief Диапазон коэффициентов PID.
 * @note ВРЕМЕННЫЙ (масштаб/единицы Kp/Ki/Kd ещё не определены) — уточнить позже.
 */
#define SETTINGS_PID_MIN  (0U)
#define SETTINGS_PID_MAX  (10000U)  /* ВРЕМЕННО, уточнить */

/**
 * @brief Номер пресета
 */
typedef enum {
    PRESET_1 = 0,
    PRESET_2,
    PRESET_3,
    PRESET_COUNT
} preset_id_t;

/**
 * @brief Результат Settings_Load() — три разных причины, а не один bool
 *
 * Разделены специально: IO_ERROR и INVALID требуют разной реакции наверху
 * (например, будущего Error-модуля). IO_ERROR — микросхема не отвечает,
 * с данными в EEPROM всё может быть в порядке, просто сейчас не достучаться.
 * INVALID — связь рабочая, но то, что там лежит, не заслуживает доверия
 * (magic/checksum не сошлись — новый/заменённый чип, либо повреждение;
 * или checksum сошёлся, но конкретное поле вне допустимого диапазона —
 * редкое, но реальное совпадение). НЕ называем это "CORRUPTED" — на
 * чистом/новом чипе magic просто не совпадёт с ожидаемым, это не
 * повреждение данных, а их отсутствие.
 */
typedef enum {
    SETTINGS_LOAD_OK = 0,      /**< Загружены сохранённые значения, все поля валидны */
    SETTINGS_LOAD_INVALID,     /**< Данные невалидны (или частично) — использованы дефолты/клампинг; связь с чипом при этом рабочая */
    SETTINGS_LOAD_IO_ERROR     /**< Ошибка связи по I2C — чип не отвечает, использованы дефолты в RAM */
} SettingsLoadStatus_t;

/**
 * @brief Установить значения по умолчанию всем каналам (не трогает EEPROM)
 *
 * Дефолты: preSet1=300, preSet2=350, preSet3=450 °C (оба инструмента),
 * target=300 (=preSet1), presleepTemp=150 °C, preSleepTimeout=sleepTimeout=0 мин
 * (sleep выключен, пока логика не реализована), Kp=Ki=Kd=0 (регулятор
 * неактивен, пока не настроен), slope/bias — номиналы из формулы датчика
 * (72 / 217, см. SETTINGS_SLOPE_SCALE/SETTINGS_BIAS_SCALE), flags=0.
 * Значения без явного задания от пользователя помечены в реализации как
 * временные — подлежат уточнению.
 */
void Settings_Init(void);

/** @brief Значение пресета канала, °C. Клампится к [SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX]. */
void     Settings_SetPreset(channel_id_t ch, preset_id_t preset, uint16_t value);
uint16_t Settings_GetPreset(channel_id_t ch, preset_id_t preset);

/**
 * @brief Текущая активная уставка канала, °C. Клампится к [SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX].
 *
 * Только хранение значения. Синхронизация с setpoint_temp в State (fixed_t)
 * — забота вызывающего кода (Input/UI), см. комментарий в шапке файла.
 */
void     Settings_SetTarget(channel_id_t ch, uint16_t value);
uint16_t Settings_GetTarget(channel_id_t ch);

/* ---- Presleep-температура ---- */

/** @brief Температура Sleep канала, °C. Клампится к [SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX]. */
void     Settings_SetPresleepTemp(channel_id_t ch, uint16_t value);
uint16_t Settings_GetPresleepTemp(channel_id_t ch);

/* ---- Калибровка датчика (RTD) ---- */

/**
 * @brief Коэффициент наклона (Slope) линеаризации канала.
 * @note Хранится умноженным на SETTINGS_SLOPE_SCALE. Клампится к
 *       [SETTINGS_SLOPE_MIN, SETTINGS_SLOPE_MAX] (диапазон временный).
 */
void     Settings_SetSlope(channel_id_t ch, uint16_t value);
uint16_t Settings_GetSlope(channel_id_t ch);

/**
 * @brief Коэффициент смещения (Bias) линеаризации канала.
 * @note Хранится умноженным на SETTINGS_BIAS_SCALE. Клампится к
 *       [SETTINGS_BIAS_MIN, SETTINGS_BIAS_MAX] (диапазон временный).
 */
void     Settings_SetBias(channel_id_t ch, uint16_t value);
uint16_t Settings_GetBias(channel_id_t ch);

/* ---- Тайминги Sleep ---- */

/** @brief Время до Pre-Sleep, минуты. Клампится к [0, SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES]. */
void     Settings_SetPreSleepTimeout(channel_id_t ch, uint16_t value);
uint16_t Settings_GetPreSleepTimeout(channel_id_t ch);

/** @brief Время до Sleep, минуты. Клампится к [0, SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES]. */
void     Settings_SetSleepTimeout(channel_id_t ch, uint16_t value);
uint16_t Settings_GetSleepTimeout(channel_id_t ch);

/* ---- Коэффициенты PID ---- */

/** @brief Kp PID канала. Клампится к [SETTINGS_PID_MIN, SETTINGS_PID_MAX] (временно). */
void     Settings_SetKp(channel_id_t ch, uint16_t value);
uint16_t Settings_GetKp(channel_id_t ch);

/** @brief Ki PID канала. Клампится к [SETTINGS_PID_MIN, SETTINGS_PID_MAX] (временно). */
void     Settings_SetKi(channel_id_t ch, uint16_t value);
uint16_t Settings_GetKi(channel_id_t ch);

/** @brief Kd PID канала. Клампится к [SETTINGS_PID_MIN, SETTINGS_PID_MAX] (временно). */
void     Settings_SetKd(channel_id_t ch, uint16_t value);
uint16_t Settings_GetKd(channel_id_t ch);

/* ---- Глобальные конфигурационные флаги (не по каналам) ---- */

/** @brief Весь байт флагов целиком (удобно для записи/чтения EEPROM одним блоком) */
void    Settings_SetFlags(uint8_t value);
uint8_t Settings_GetFlags(void);

/**
 * @brief Один бит флагов
 * @param bit_index 0..7
 */
void Settings_SetFlagBit(uint8_t bit_index, bool value);
bool Settings_GetFlagBit(uint8_t bit_index);

/* ---- Персист в EEPROM (все поля выше, всех каналов) ---- */

/**
 * @brief Немедленно сохранить весь конфиг в EEPROM
 *
 * Обычно не нужно звать напрямую — см. Settings_Poll() для отложенного
 * автосохранения. Полезна, если нужно гарантированно сохранить прямо
 * сейчас (например, по сигналу о пропадании питания).
 *
 * @return true — записано успешно; false — сбой шины/микросхемы
 */
bool Settings_Save(void);

/**
 * @brief Вызывать периодически из главного цикла
 *
 * Если было изменение (любым Settings_Set*) и с него прошло не менее
 * SETTINGS_SAVE_DELAY_MS без новых изменений — выполняет Settings_Save().
 * Не блокирует, если сохранять нечего (просто сравнение по HAL_GetTick()).
 */
void Settings_Poll(void);

/**
 * @brief Загрузить конфиг из EEPROM в RAM. Вызывать один раз при старте.
 *
 * Поведение по типу сбоя:
 *  - Ошибка связи по I2C (микросхема не отвечает) — в RAM остаются/
 *    устанавливаются значения по умолчанию (Settings_Init()), в EEPROM
 *    ничего не пишется (бессмысленно при нерабочей шине).
 *    → SETTINGS_LOAD_IO_ERROR.
 *  - Связь успешна, но magic/checksum не сошлись (заменили микросхему,
 *    чистый чип, либо данные повреждены) — чип полностью стирается
 *    (заполняется 0xFF) и в него пишутся значения по умолчанию.
 *    → SETTINGS_LOAD_INVALID (даже если запись дефолтов обратно прошла
 *    успешно — сам факт невалидности сохраняется в результате, а не
 *    "гасится" последующим успешным восстановлением; ожидается, что
 *    вызывающий код — например, будущий Error-модуль — покажет
 *    предупреждение об этом до конца текущего сеанса).
 *  - Связь успешна, magic/checksum сошлись, НО хотя бы одно поле вне
 *    своего допустимого диапазона (редкое, но реальное совпадение
 *    повреждённых байт с верной checksum) — поле молча зажимается к
 *    границе (те же диапазоны, что и у Settings_Set*()), а исправленные
 *    значения ставятся на отложенную запись обратно в EEPROM.
 *    → SETTINGS_LOAD_INVALID (по той же логике — данные лежавшие в EEPROM
 *    не заслуживали доверия, факт этого не должен теряться).
 *
 * @return см. SettingsLoadStatus_t
 */
SettingsLoadStatus_t Settings_Load(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
