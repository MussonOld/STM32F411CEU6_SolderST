/**
 * @file channel.h
 * @brief Общий идентификатор канала станции (Solder/Desolder).
 *
 * Вынесен в Common, а не в state.h, чтобы независимые модули (Settings и
 * т.п.) могли использовать его, не таща за собой зависимость от State.
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHANNEL_SOLDER = 0,
    CHANNEL_DESOLDER,
    CHANNEL_COUNT
} channel_id_t;

#ifdef __cplusplus
}
#endif

#endif /* CHANNEL_H */
