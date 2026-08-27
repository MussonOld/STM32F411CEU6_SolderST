/**
 * @file settings.c
 * @brief Реализация settings.h — клампинг диапазонов, отложенная запись в
 *        EEPROM, обработка невалидных/недоступных данных при загрузке.
 */

#include <stdbool.h>
#include "settings.h"
#include "eeprom.h"
#include "stm32f4xx_hal.h" /* HAL_GetTick() — для таймера отложенной записи */

/**
 * @brief Данные одного канала. Пишутся/читаются только из основного цикла
 *        (Input/UI-слой) — никаких ISR не трогает эти поля, volatile не нужен.
 */
typedef struct {
    uint16_t preset[PRESET_COUNT];
    uint16_t target;
    uint16_t presleep_temp;
    uint16_t slope;
    uint16_t bias;
    uint16_t pre_sleep_timeout;
    uint16_t sleep_timeout;
    uint16_t kp;
    uint16_t ki;
    uint16_t kd;
} channel_settings_t;

static channel_settings_t s_channels[CHANNEL_COUNT];
static uint8_t s_flags;

/* ---- Отложенная запись ---- */
static bool     s_dirty = false;
static uint32_t s_last_change_tick = 0;

/* ---- Разметка EEPROM ----
 * addr 0-1 : magic (0xA55A) — пишется последним при сохранении, служит
 *            признаком "данные записаны полностью"; если питание пропало
 *            посреди записи, magic не совпадёт и Settings_Load() уйдёт
 *            в значения по умолчанию, а не прочитает половину нового +
 *            половину старого блока.
 * addr 2   : контрольная сумма (8-бит сумма всех байт данных)
 * addr 3   : зарезервировано (не используется, пишется 0)
 * addr 4.. : данные — по CHANNEL_COUNT блоков по SETTINGS_EEPROM_BYTES_PER_CH
 *            байт (preset x3, target, presleep_temp, slope, bias,
 *            pre_sleep_timeout, sleep_timeout, kp, ki, kd — по 2 байта, LE),
 *            блок CHANNEL_SOLDER первый, затем CHANNEL_DESOLDER; после
 *            обоих блоков — 1 байт flags.
 */
#define SETTINGS_EEPROM_MAGIC          (0xA55AU)
#define SETTINGS_EEPROM_ADDR_MAGIC     (0U)
#define SETTINGS_EEPROM_ADDR_CHECKSUM  (2U)
#define SETTINGS_EEPROM_ADDR_RESERVED  (3U)
#define SETTINGS_EEPROM_ADDR_DATA      (4U)
#define SETTINGS_EEPROM_FIELDS_PER_CH  (12U) /* preset x3 + target + presleep_temp + slope + bias + pre_sleep_timeout + sleep_timeout + kp + ki + kd */
#define SETTINGS_EEPROM_BYTES_PER_CH   (SETTINGS_EEPROM_FIELDS_PER_CH * 2U)
#define SETTINGS_EEPROM_CH_DATA_LEN    (CHANNEL_COUNT * SETTINGS_EEPROM_BYTES_PER_CH)
#define SETTINGS_EEPROM_FLAGS_LEN      (1U)
#define SETTINGS_EEPROM_DATA_LEN       (SETTINGS_EEPROM_CH_DATA_LEN + SETTINGS_EEPROM_FLAGS_LEN)

/* ---- Значения по умолчанию — единственный источник истины, используются
 * и Settings_Init() (полный сброс/первый старт/INVALID EEPROM), и точечными
 * Settings_Reset*Defaults() (пункт "Сброс" сервисного меню), чтобы они не
 * могли разъехаться при будущих правках дефолтов. */
#define SETTINGS_DEFAULT_PRESET_1           (300U)
#define SETTINGS_DEFAULT_PRESET_2           (350U)
#define SETTINGS_DEFAULT_PRESET_3           (450U)
#define SETTINGS_DEFAULT_TARGET             (SETTINGS_DEFAULT_PRESET_1) /* не задано явно пользователем */
#define SETTINGS_DEFAULT_PRESLEEP_TEMP      (150U)
#define SETTINGS_DEFAULT_PRE_SLEEP_TIMEOUT  (5U)
#define SETTINGS_DEFAULT_SLEEP_TIMEOUT      (5U)
#define SETTINGS_DEFAULT_SLOPE              (72U)  /* номинал 0.072 * SETTINGS_SLOPE_SCALE */
#define SETTINGS_DEFAULT_BIAS               (217U) /* номинал 21.7 * SETTINGS_BIAS_SCALE */
#define SETTINGS_DEFAULT_KP                 (0U)
#define SETTINGS_DEFAULT_KI                 (0U)
#define SETTINGS_DEFAULT_KD                 (0U)

static inline bool channel_valid(channel_id_t ch)
{
    return (ch >= 0) && (ch < CHANNEL_COUNT);
}

static inline bool preset_valid(preset_id_t preset)
{
    return (preset >= 0) && (preset < PRESET_COUNT);
}

static inline uint16_t clamp_u16(uint16_t value, uint16_t lo, uint16_t hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/**
 * @brief Прогнать значения ОДНОГО канала через те же диапазоны, что и
 *        публичные Settings_Set*() (см. settings.h) — молча зажимает к
 *        границе, не отбрасывает.
 *
 * Нужна ОТДЕЛЬНО от самих сеттеров: unpack_data() (см. ниже, вызывается из
 * Settings_Load() после проверки magic+checksum) пишет поля НАПРЯМУЮ в
 * структуру, в обход сеттеров и их клампинга — корректный checksum сам по
 * себе не гарантирует, что КАЖДОЕ отдельное поле лежит в допустимом
 * диапазоне (совпадение checksum на частично повреждённых данных редкое,
 * но реальное; это persistent-конфиг, включая slope/bias/Kp-Ki-Kd/лимиты
 * температуры — молча доверять сырым байтам здесь недопустимо).
 *
 * @return true, если хоть одно поле реально изменилось (было вне диапазона)
 */
static bool clamp_channel(channel_settings_t *c)
{
    bool changed = false;
#define CLAMP_FIELD(field, lo, hi)                              \
    do {                                                        \
        uint16_t clamped_ = clamp_u16((c)->field, (lo), (hi));  \
        if (clamped_ != (c)->field) { (c)->field = clamped_; changed = true; } \
    } while (0)

    CLAMP_FIELD(preset[PRESET_1],  SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    CLAMP_FIELD(preset[PRESET_2],  SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    CLAMP_FIELD(preset[PRESET_3],  SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    CLAMP_FIELD(target,            SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    CLAMP_FIELD(presleep_temp,     SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    CLAMP_FIELD(slope,             SETTINGS_SLOPE_MIN, SETTINGS_SLOPE_MAX);
    CLAMP_FIELD(bias,              SETTINGS_BIAS_MIN, SETTINGS_BIAS_MAX);
    CLAMP_FIELD(pre_sleep_timeout, SETTINGS_SLEEP_TIMEOUT_MIN_MINUTES, SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES);
    CLAMP_FIELD(sleep_timeout,     SETTINGS_SLEEP_TIMEOUT_MIN_MINUTES, SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES);
    CLAMP_FIELD(kp,                SETTINGS_PID_MIN, SETTINGS_PID_MAX);
    CLAMP_FIELD(ki,                SETTINGS_PID_MIN, SETTINGS_PID_MAX);
    CLAMP_FIELD(kd,                SETTINGS_PID_MIN, SETTINGS_PID_MAX);

#undef CLAMP_FIELD
    return changed;
}

/**
 * @brief Взвести таймер отложенной записи, если значение реально изменилось
 * @return "новое" значение после клампа (для удобного использования в сеттерах)
 */
static void mark_dirty_if_changed(uint16_t old_value, uint16_t new_value)
{
    if (old_value != new_value) {
        s_dirty = true;
        s_last_change_tick = HAL_GetTick();
    }
}

void Settings_Init(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_channels[ch].preset[PRESET_1]  = SETTINGS_DEFAULT_PRESET_1;
        s_channels[ch].preset[PRESET_2]  = SETTINGS_DEFAULT_PRESET_2;
        s_channels[ch].preset[PRESET_3]  = SETTINGS_DEFAULT_PRESET_3;
        s_channels[ch].target            = SETTINGS_DEFAULT_TARGET;
        s_channels[ch].presleep_temp     = SETTINGS_DEFAULT_PRESLEEP_TEMP;
        s_channels[ch].slope             = SETTINGS_DEFAULT_SLOPE;
        s_channels[ch].bias              = SETTINGS_DEFAULT_BIAS;
        s_channels[ch].pre_sleep_timeout = SETTINGS_DEFAULT_PRE_SLEEP_TIMEOUT;
        s_channels[ch].sleep_timeout     = SETTINGS_DEFAULT_SLEEP_TIMEOUT;
        s_channels[ch].kp                = SETTINGS_DEFAULT_KP;
        s_channels[ch].ki                = SETTINGS_DEFAULT_KI;
        s_channels[ch].kd                = SETTINGS_DEFAULT_KD;
    }
    s_flags = 0;

    s_dirty = false; /* дефолты ещё не считаются "изменением" — не пишем в EEPROM сами по себе */
}

void Settings_SetPreset(channel_id_t ch, preset_id_t preset, uint16_t value)
{
    if (!channel_valid(ch) || !preset_valid(preset)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    mark_dirty_if_changed(s_channels[ch].preset[preset], clamped);
    s_channels[ch].preset[preset] = clamped;
}

uint16_t Settings_GetPreset(channel_id_t ch, preset_id_t preset)
{
    if (!channel_valid(ch) || !preset_valid(preset)) return 0;
    return s_channels[ch].preset[preset];
}

void Settings_SetTarget(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    mark_dirty_if_changed(s_channels[ch].target, clamped);
    s_channels[ch].target = clamped;
}

uint16_t Settings_GetTarget(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].target;
}

/* ---- Presleep-температура ---- */

void Settings_SetPresleepTemp(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_TEMP_MIN, SETTINGS_TEMP_MAX);
    mark_dirty_if_changed(s_channels[ch].presleep_temp, clamped);
    s_channels[ch].presleep_temp = clamped;
}

uint16_t Settings_GetPresleepTemp(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].presleep_temp;
}

/* ---- Калибровка (Slope/Bias) ---- */

void Settings_SetSlope(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_SLOPE_MIN, SETTINGS_SLOPE_MAX);
    mark_dirty_if_changed(s_channels[ch].slope, clamped);
    s_channels[ch].slope = clamped;
}

uint16_t Settings_GetSlope(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].slope;
}

void Settings_SetBias(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_BIAS_MIN, SETTINGS_BIAS_MAX);
    mark_dirty_if_changed(s_channels[ch].bias, clamped);
    s_channels[ch].bias = clamped;
}

uint16_t Settings_GetBias(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].bias;
}

/* ---- Тайминги Sleep ---- */

void Settings_SetPreSleepTimeout(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_SLEEP_TIMEOUT_MIN_MINUTES, SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES);
    mark_dirty_if_changed(s_channels[ch].pre_sleep_timeout, clamped);
    s_channels[ch].pre_sleep_timeout = clamped;
}

uint16_t Settings_GetPreSleepTimeout(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].pre_sleep_timeout;
}

void Settings_SetSleepTimeout(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_SLEEP_TIMEOUT_MIN_MINUTES, SETTINGS_SLEEP_TIMEOUT_MAX_MINUTES);
    mark_dirty_if_changed(s_channels[ch].sleep_timeout, clamped);
    s_channels[ch].sleep_timeout = clamped;
}

uint16_t Settings_GetSleepTimeout(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].sleep_timeout;
}

/* ---- Коэффициенты PID ---- */

void Settings_SetKp(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_PID_MIN, SETTINGS_PID_MAX);
    mark_dirty_if_changed(s_channels[ch].kp, clamped);
    s_channels[ch].kp = clamped;
}

uint16_t Settings_GetKp(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].kp;
}

void Settings_SetKi(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_PID_MIN, SETTINGS_PID_MAX);
    mark_dirty_if_changed(s_channels[ch].ki, clamped);
    s_channels[ch].ki = clamped;
}

uint16_t Settings_GetKi(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].ki;
}

void Settings_SetKd(channel_id_t ch, uint16_t value)
{
    if (!channel_valid(ch)) return;
    uint16_t clamped = clamp_u16(value, SETTINGS_PID_MIN, SETTINGS_PID_MAX);
    mark_dirty_if_changed(s_channels[ch].kd, clamped);
    s_channels[ch].kd = clamped;
}

uint16_t Settings_GetKd(channel_id_t ch)
{
    if (!channel_valid(ch)) return 0;
    return s_channels[ch].kd;
}

/* ---- Глобальные флаги ---- */

void Settings_SetFlags(uint8_t value)
{
    if (s_flags != value) {
        s_dirty = true;
        s_last_change_tick = HAL_GetTick();
    }
    s_flags = value;
}

uint8_t Settings_GetFlags(void)
{
    return s_flags;
}

void Settings_SetFlagBit(uint8_t bit_index, bool value)
{
    if (bit_index > 7) return;
    uint8_t new_flags = s_flags;
    if (value) {
        new_flags |= (uint8_t)(1u << bit_index);
    } else {
        new_flags &= (uint8_t)~(1u << bit_index);
    }
    if (new_flags != s_flags) {
        s_dirty = true;
        s_last_change_tick = HAL_GetTick();
    }
    s_flags = new_flags;
}

bool Settings_GetFlagBit(uint8_t bit_index)
{
    if (bit_index > 7) return false;
    return (s_flags & (uint8_t)(1u << bit_index)) != 0;
}

/* ---- Персист в EEPROM ---- */

/**
 * @brief Сериализовать все каналы + flags в буфер (little-endian)
 * @param buf Буфер длиной не менее SETTINGS_EEPROM_DATA_LEN
 */
static void pack_data(uint8_t *buf)
{
    uint16_t offset = 0;

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint16_t fields[SETTINGS_EEPROM_FIELDS_PER_CH] = {
            s_channels[ch].preset[PRESET_1],
            s_channels[ch].preset[PRESET_2],
            s_channels[ch].preset[PRESET_3],
            s_channels[ch].target,
            s_channels[ch].presleep_temp,
            s_channels[ch].slope,
            s_channels[ch].bias,
            s_channels[ch].pre_sleep_timeout,
            s_channels[ch].sleep_timeout,
            s_channels[ch].kp,
            s_channels[ch].ki,
            s_channels[ch].kd,
        };

        for (int f = 0; f < SETTINGS_EEPROM_FIELDS_PER_CH; f++) {
            buf[offset]     = (uint8_t)(fields[f] & 0xFF);
            buf[offset + 1] = (uint8_t)(fields[f] >> 8);
            offset += 2;
        }
    }

    buf[offset] = s_flags;
}

/**
 * @brief Разобрать буфер (little-endian) обратно в s_channels/s_flags
 */
static void unpack_data(const uint8_t *buf)
{
    uint16_t offset = 0;

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        uint16_t fields[SETTINGS_EEPROM_FIELDS_PER_CH];

        for (int f = 0; f < SETTINGS_EEPROM_FIELDS_PER_CH; f++) {
            fields[f] = (uint16_t)(buf[offset] | ((uint16_t)buf[offset + 1] << 8));
            offset += 2;
        }

        s_channels[ch].preset[PRESET_1]  = fields[0];
        s_channels[ch].preset[PRESET_2]  = fields[1];
        s_channels[ch].preset[PRESET_3]  = fields[2];
        s_channels[ch].target            = fields[3];
        s_channels[ch].presleep_temp        = fields[4];
        s_channels[ch].slope             = fields[5];
        s_channels[ch].bias              = fields[6];
        s_channels[ch].pre_sleep_timeout = fields[7];
        s_channels[ch].sleep_timeout     = fields[8];
        s_channels[ch].kp                = fields[9];
        s_channels[ch].ki                = fields[10];
        s_channels[ch].kd                = fields[11];
    }

    s_flags = buf[offset];
}

static uint8_t checksum8(const uint8_t *buf, uint16_t len)
{
    uint8_t sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);
    }
    return sum;
}

bool Settings_Save(void)
{
    uint8_t data[SETTINGS_EEPROM_DATA_LEN];
    pack_data(data);

    uint8_t checksum = checksum8(data, SETTINGS_EEPROM_DATA_LEN);

    /* Порядок важен: сперва данные и контрольная сумма, magic — последним.
     * Так при потере питания посреди записи magic не совпадёт при следующей
     * загрузке, и Settings_Load() корректно откатится к умолчаниям вместо
     * чтения наполовину нового/наполовину старого блока. */
    if (EEPROM_Write(SETTINGS_EEPROM_ADDR_DATA, data, SETTINGS_EEPROM_DATA_LEN) != EEPROM_OK) {
        return false;
    }
    if (EEPROM_WriteByte(SETTINGS_EEPROM_ADDR_CHECKSUM, checksum) != EEPROM_OK) {
        return false;
    }

    uint8_t magic_bytes[2] = {
        (uint8_t)(SETTINGS_EEPROM_MAGIC & 0xFF),
        (uint8_t)(SETTINGS_EEPROM_MAGIC >> 8),
    };
    if (EEPROM_Write(SETTINGS_EEPROM_ADDR_MAGIC, magic_bytes, 2) != EEPROM_OK) {
        return false;
    }

    s_dirty = false;
    return true;
}

void Settings_Poll(void)
{
    if (!s_dirty) {
        return;
    }
    if ((HAL_GetTick() - s_last_change_tick) >= SETTINGS_SAVE_DELAY_MS) {
        Settings_Save(); /* при неудаче s_dirty намеренно остаётся true — попробуем на следующем Poll */
    }
}

/**
 * @brief Заполнить весь чип 0xFF ("стереть")
 */
static bool erase_eeprom(void)
{
    uint8_t blank[32];
    for (uint16_t i = 0; i < sizeof(blank); i++) {
        blank[i] = 0xFF;
    }

    uint16_t size = EEPROM_GetSize();
    uint16_t addr = 0;

    while (addr < size) {
        uint16_t chunk = (uint16_t)((size - addr < sizeof(blank)) ? (size - addr) : sizeof(blank));
        if (EEPROM_Write(addr, blank, chunk) != EEPROM_OK) {
            return false;
        }
        addr = (uint16_t)(addr + chunk);
    }

    return true;
}

SettingsLoadStatus_t Settings_Load(void)
{
    uint8_t magic_bytes[2];
    if (EEPROM_Read(SETTINGS_EEPROM_ADDR_MAGIC, magic_bytes, 2) != EEPROM_OK) {
        /* Ошибка связи по I2C — просто работаем с дефолтами в RAM, ничего
         * не пишем обратно (бессмысленно/рискованно при нерабочей шине) */
        Settings_Init();
        return SETTINGS_LOAD_IO_ERROR;
    }

    uint16_t magic = (uint16_t)(magic_bytes[0] | ((uint16_t)magic_bytes[1] << 8));

    bool data_valid = false;
    if (magic == SETTINGS_EEPROM_MAGIC) {
        uint8_t stored_checksum;
        uint8_t data[SETTINGS_EEPROM_DATA_LEN];

        if (EEPROM_ReadByte(SETTINGS_EEPROM_ADDR_CHECKSUM, &stored_checksum) == EEPROM_OK &&
            EEPROM_Read(SETTINGS_EEPROM_ADDR_DATA, data, SETTINGS_EEPROM_DATA_LEN) == EEPROM_OK &&
            checksum8(data, SETTINGS_EEPROM_DATA_LEN) == stored_checksum) {
            unpack_data(data);
            data_valid = true;
        }
    }

    if (data_valid) {
        bool any_clamped = false;
        for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
            if (clamp_channel(&s_channels[ch])) {
                any_clamped = true;
            }
        }
        if (any_clamped) {
            /* Checksum сошёлся, но хотя бы одно поле оказалось вне
             * допустимого диапазона (редкое, но реальное совпадение
             * повреждённых байт с верной контрольной суммой) — молча
             * зажали к границе; взводим отложенную запись, чтобы при
             * следующем Settings_Poll() поверх EEPROM легли уже
             * безопасные значения, а не остались сырые. Данные всё
             * равно считаются невалидными — сам факт клампинга не
             * "гасится" последующим исправлением. */
            s_dirty = true;
            s_last_change_tick = HAL_GetTick();
            return SETTINGS_LOAD_INVALID;
        }
        s_dirty = false;
        return SETTINGS_LOAD_OK;
    }

    /* Magic не совпал или контрольная сумма не сошлась — связь с чипом
     * рабочая, но данные не наши/повреждены (например, заменили микросхему).
     * Стираем чип целиком и пишем значения по умолчанию. Результат — INVALID
     * независимо от того, успешно ли записались дефолты обратно: сам факт,
     * что сохранённых данных не было/они не годились, должен долететь до
     * вызывающего кода, а не потеряться за успешным восстановлением. */
    Settings_Init();
    erase_eeprom();
    Settings_Save();
    return SETTINGS_LOAD_INVALID;
}

bool Settings_ResetToDefaults(void)
{
    /* Тот же путь восстановления, что и при обнаружении невалидных данных
     * в Settings_Load() (см. там) — стереть чип целиком, поднять дефолты в
     * RAM, записать их обратно. Публичная обёртка — нужна пункту "Сброс"
     * сервисного меню (Expert). */
    Settings_Init();
    bool erased = erase_eeprom();
    bool saved = Settings_Save();
    return erased && saved;
}

void Settings_ResetUserDefaults(channel_id_t ch)
{
    if (!channel_valid(ch)) return;

    Settings_SetPresleepTemp(ch, SETTINGS_DEFAULT_PRESLEEP_TEMP);
    Settings_SetPreSleepTimeout(ch, SETTINGS_DEFAULT_PRE_SLEEP_TIMEOUT);
    Settings_SetSleepTimeout(ch, SETTINGS_DEFAULT_SLEEP_TIMEOUT);
    Settings_SetFlagBit(SETTINGS_FLAG_BUZZER_BIT, false); /* глобальный флаг — см. докстринг в settings.h */
}

void Settings_ResetExpertDefaults(channel_id_t ch)
{
    if (!channel_valid(ch)) return;

    Settings_SetKp(ch, SETTINGS_DEFAULT_KP);
    Settings_SetKi(ch, SETTINGS_DEFAULT_KI);
    Settings_SetKd(ch, SETTINGS_DEFAULT_KD);
    Settings_SetSlope(ch, SETTINGS_DEFAULT_SLOPE);
    Settings_SetBias(ch, SETTINGS_DEFAULT_BIAS);
}
