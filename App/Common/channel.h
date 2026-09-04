/**
 * @file channel.h
 * @brief Общий идентификатор канала станции (Solder/Desolder).
 *
 * Вынесен в Common, а не в state.h, чтобы независимые модули (Settings и
 * т.п.) могли использовать его, не таща за собой зависимость от State.
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHANNEL_SOLDER = 0,
    CHANNEL_DESOLDER,
    CHANNEL_COUNT
} channel_id_t;

/**
 * @brief Bounds-check идентификатора канала. Общая для всех модулей,
 *        работающих с массивами по CHANNEL_COUNT (State/Settings/Error/Sleep).
 */
static inline bool channel_valid(channel_id_t ch)
{
    return (ch >= 0) && (ch < CHANNEL_COUNT);
}

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_H */
