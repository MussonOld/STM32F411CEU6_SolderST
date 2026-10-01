/**
 * @file    screen_channel.h
 * @brief   Отрисовка каналов главного экрана (App/Screen, внутренний модуль):
 *          содержимое CURRENT-блока (число/авария/"ВЫКЛ"/крестик/иконки сна),
 *          уставка, гейдж мощности, цвета активного канала и таймер сна в
 *          инфозоне. Публичный вход экрана — screen.h; сюда ходит только screen.c.
 *
 * Порядок вызовов в Screen_Update() важен для порядка обращений к TextField
 * (проверяется tests/screen_golden), поэтому обновление разбито на три шага,
 * между которыми screen.c рисует общие поля (пресеты, инфо-сообщение).
 */
#ifndef SCREEN_CHANNEL_H
#define SCREEN_CHANNEL_H

#include "channel.h"

/** @brief Сконфигурировать строки TextField инфозоны-таймеров сна и обоих каналов; начальное состояние. Вызывать из Screen_Init(). */
void ScreenChannel_Init(void);

/** @brief Забыть, что нарисовано растрами/гейджем (экран только что залит фоном целиком — смена режима, заставка). */
void ScreenChannel_ResetDrawnState(void);

/** @brief Шаг 1: содержимое обоих каналов (число/авария/иконки/уставка) и гейджи мощности. */
void ScreenChannel_UpdateContent(void);

/** @brief Шаг 2: если активный канал сменился — перекрасить поля (после обновления пресетов). */
void ScreenChannel_UpdateActiveColors(channel_id_t active);

/** @brief Шаг 3: таймеры сна обоих каналов в инфозоне (текст, цвет, иконка циферблата). */
void ScreenChannel_UpdateSleepStatus(void);

#endif /* SCREEN_CHANNEL_H */
