/**
 * @file control_log.h
 * @brief Формат RAM-лога нагрева (отладка подбора PID). Компилируется в
 *        прошивку только при CONTROL_LOG_ENABLE=1 (см. control.h); реализация
 *        — в control.c, раскодировщик — tools/control_log_decode.py.
 *
 * Буфер лежит по ФИКСИРОВАННОМУ адресу CONTROL_LOG_ADDR (регион LOG в обоих
 * линкер-скриптах) и вычитывается через ST-Link без остановки программы:
 *
 *   STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r8 0x2001A000 0x5E00 log.bin
 *
 * (в GUI STM32CubeProgrammer: Mode = Hot plug, вкладка Memory, адрес 0x2001A000,
 * размер 0x5E00, Save as .bin).
 *
 * Раскладка: control_log_header_t (64 байта) + CONTROL_LOG_CAPACITY записей
 * control_log_rec_t по 20 байт, little-endian, без выравнивающих дыр.
 *
 * Единицы в записи: температуры — 0.1 °C, проценты — 0.1 %. Слагаемые
 * регулятора (p/i/d/ff) — ДО клампа в [0,100] %, их сумма — сырой выход PID;
 * power — то, что реально ушло в ШИМ (после клампа и округления).
 */

#ifndef CONTROL_LOG_H
#define CONTROL_LOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONTROL_LOG_ADDR       0x2001A000UL   /* начало региона LOG в *.ld */
#define CONTROL_LOG_REGION_SZ  0x6000UL       /* размер региона LOG в *.ld (24 КБ) */
#define CONTROL_LOG_CAPACITY   1200U          /* 60 с при 20 шагах PID в секунду */
#define CONTROL_LOG_MAGIC      0x474F4C43UL   /* 'C','L','O','G' в little-endian */
#define CONTROL_LOG_VERSION    1U

/* header.state */
#define CONTROL_LOG_ARMED      0U   /* ждёт старта нагрева; запись в 0 (SWD) взводит заново */
#define CONTROL_LOG_RECORDING  1U
#define CONTROL_LOG_FULL       2U   /* заполнен; дальше не пишется до сброса/взвода */

/* rec.flags */
#define CLOG_F_STAGE1     (1U << 0)  /* ПИД гонит на промежуточный рубеж (sp_work < sp) */
#define CLOG_F_IN_BAND    (1U << 1)  /* |sp - temp| <= CONTROL_INTEGRAL_BAND_C: интеграл и FF активны */
#define CLOG_F_NEAR       (1U << 2)  /* |sp - temp| <= CONTROL_POWER_DISPLAY_STEADY_BAND_C */
#define CLOG_F_FF_FADED   (1U << 3)  /* температура растёт (dT/dt > 0): FF частично/полностью убран */
#define CLOG_F_SAT_LO     (1U << 4)  /* p+i+d+ff < 0, выход заклампирован в 0 */
#define CLOG_F_SAT_HI     (1U << 5)  /* p+i+d+ff > 100 %, выход заклампирован в 100 */
#define CLOG_F_I_CLAMP    (1U << 6)  /* интеграл упёрт в i_max = 100/Ki */
#define CLOG_F_OVERSHOOT  (1U << 7)  /* temp выше настоящей уставки (работает гашение интеграла) */
#define CLOG_F_PRESLEEP   (1U << 8)  /* режим PRESLEEP (уставка = min(уставка, PresleepTemp)) */
#define CLOG_F_FIRST      (1U << 9)  /* первая запись после (пере)старта записи */

#pragma pack(push, 1)

typedef struct {
    uint16_t t_ms;     /* младшие 16 бит тика отсчёта АЦП (HAL_GetTick), мс; период 65.5 с > 60 с окна */
    int16_t  temp;     /* измеренная температура, 0.1 °C */
    int16_t  sp;       /* настоящая (эффективная) уставка, 0.1 °C */
    int16_t  sp_work;  /* рабочая цель текущей стадии (двухстадийная уставка), 0.1 °C */
    int16_t  power;    /* выход PID после клампа, 0.1 % (шаг 10 = 1 %) */
    int16_t  p;        /*  Kp*e,        0.1 %, до клампа, насыщается по int16 */
    int16_t  i;        /*  Ki*I,        0.1 % */
    int16_t  d;        /* -Kd*dT/dt,    0.1 % (знак уже учтён) */
    int16_t  ff;       /*  feed-forward, 0.1 % */
    uint16_t flags;    /* CLOG_F_* */
} control_log_rec_t;   /* 20 байт */

typedef struct {
    uint32_t magic;        /* CONTROL_LOG_MAGIC */
    uint16_t version;      /* CONTROL_LOG_VERSION */
    uint16_t rec_size;     /* sizeof(control_log_rec_t) */
    uint16_t capacity;     /* CONTROL_LOG_CAPACITY */
    uint16_t count;        /* число ВАЛИДНЫХ записей (обновляется после записи) */
    uint16_t state;        /* CONTROL_LOG_ARMED/RECORDING/FULL */
    uint8_t  channel;      /* channel_id_t логируемого канала */
    uint8_t  reserved0;
    uint16_t kp, ki, kd;   /* сырые коэффициенты на момент (пере)старта записи */
    uint16_t pid_scale;    /* CONTROL_PID_SCALE: real = raw / pid_scale */
    uint32_t t0_ms;        /* полный HAL_GetTick() первой записи — для привязки t_ms */
    uint16_t restarts;     /* сколько раз запись перезапускалась с последнего сброса */
    /* Снимок констант регулятора (чтобы раскодировщик/модель знали, чем считали): */
    uint16_t ff_pct_per_100c;
    uint16_t ambient_c;
    uint16_t integral_band_c;
    uint16_t approach_offset_c;
    uint16_t overshoot_gain;
    uint16_t ff_fade_dtdt;
    uint16_t dtdt_filter_ms;
    uint16_t steady_band_c;
    uint16_t poll_ms;
    uint16_t pwm_period_ms;
    uint8_t  reserved1[14];
} control_log_header_t;    /* 64 байта */

typedef struct {
    control_log_header_t hdr;
    control_log_rec_t    rec[CONTROL_LOG_CAPACITY];
} control_log_t;           /* 64 + 1200*20 = 24064 = 0x5E00 байт */

#pragma pack(pop)

_Static_assert(sizeof(control_log_rec_t) == 20, "control_log_rec_t must be 20 bytes");
_Static_assert(sizeof(control_log_header_t) == 64, "control_log_header_t must be 64 bytes");
_Static_assert(sizeof(control_log_t) <= CONTROL_LOG_REGION_SZ, "log does not fit the LOG linker region");

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_LOG_H */
