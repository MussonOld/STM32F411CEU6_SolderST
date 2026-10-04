/*
 * Сквозной тест RAM-лога нагрева (хост, gcc; в прошивку не входит).
 *
 * Замкнутый контур Control_Poll() + двухмассовая тепловая модель жала: уставка 50 °C, через 20 с
 * скачок на 200 °C (в логе должен остаться именно этот разгон: перезапуск записи по скачку
 * уставки), затем ~90 с работы до заполнения буфера. Буфер сбрасывается в build/log.bin так же,
 * как его снимал бы ST-Link (sizeof(control_log_t) байт), дальше его проверяет verify.py
 * независимым раскодировщиком tools/control_log_decode.py.
 */
#define CONTROL_LOG_ENABLE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "control.c"

GPIO_TypeDef test_gpioa, test_gpiob, test_gpioc;

static uint32_t g_tick;
static bool g_heater[CHANNEL_COUNT];
static fixed_t g_setpoint[CHANNEL_COUNT], g_meas[CHANNEL_COUNT];
static uint32_t g_upd[CHANNEL_COUNT];

uint32_t HAL_GetTick(void) { return g_tick; }
void HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, GPIO_PinState st) { (void)p; (void)pin; (void)st; }
void State_SetHeaterActive(channel_id_t ch, bool a) { g_heater[ch] = a; }
bool State_IsHeaterActive(channel_id_t ch) { return g_heater[ch]; }
bool State_IsEnabled(channel_id_t ch) { return ch == CHANNEL_SOLDER; }
fixed_t State_GetSetpointTemp(channel_id_t ch) { return g_setpoint[ch]; }
void State_SetCurrentTemp(channel_id_t ch, fixed_t t) { (void)ch; (void)t; }
sleep_mode_t Sleep_GetMode(channel_id_t ch) { (void)ch; return SLEEP_MODE_AWAKE; }
bool Error_IsChannelBlocked(channel_id_t ch) { (void)ch; return false; }
bool Diag_IsSampleEvaluated(channel_id_t ch) { (void)ch; return true; }
bool ADS1220_IsDataValid(channel_id_t ch) { (void)ch; return true; }
fixed_t ADS1220_GetTemperatureC(channel_id_t ch) { return g_meas[ch]; }
uint32_t ADS1220_GetLastUpdateTick(channel_id_t ch) { return g_upd[ch]; }
uint16_t Settings_GetPresleepTemp(channel_id_t ch) { (void)ch; return 150; }
uint16_t Settings_GetKp(channel_id_t ch) { (void)ch; return 500; }
uint16_t Settings_GetKi(channel_id_t ch) { (void)ch; return 90; }
uint16_t Settings_GetKd(channel_id_t ch) { (void)ch; return 300; }

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "build/log.bin";
    double Th = 25.0, Ts = 25.0;                      /* нагревательный блок и жало, °C */
    g_setpoint[CHANNEL_SOLDER] = FIXED_FROM_INT(50);
    g_meas[CHANNEL_SOLDER] = (fixed_t)(Ts * 65536.0);
    Control_Init();

    for (g_tick = 1; g_tick <= 110000; g_tick++) {
        if (g_tick == 20000) g_setpoint[CHANNEL_SOLDER] = FIXED_FROM_INT(200);

        double u = g_heater[CHANNEL_SOLDER] ? 1.0 : 0.0;           /* 1 мс шаг модели */
        Th += (180.0 * u - 6.0 * (Th - Ts) - 0.15 * (Th - 25.0)) * 0.001;
        Ts += (4.0 * (Th - Ts)) * 0.001;

        if (g_tick % 50 == 0) {                                     /* ADS1220, 20 SPS */
            g_meas[CHANNEL_SOLDER] = (fixed_t)((Ts + ((int)((g_tick / 50) % 3) - 1) * 0.1) * 65536.0);
            g_upd[CHANNEL_SOLDER] = g_tick;
        }
        if (g_tick % CONTROL_POLL_MS == 0) Control_Poll();
    }

    const control_log_header_t *h = &g_control_log.hdr;
    printf("sim: state=%u count=%u restarts=%u Ts=%.1f\n", h->state, h->count, h->restarts, Ts);
    if (h->state != CONTROL_LOG_FULL || h->count != CONTROL_LOG_CAPACITY || h->restarts != 1) {
        fprintf(stderr, "sim: FAIL (ожидалось FULL, 1200 записей, 1 перезапуск)\n");
        return 1;
    }
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    fwrite(&g_control_log, 1, sizeof g_control_log, f);
    fclose(f);
    return 0;
}
