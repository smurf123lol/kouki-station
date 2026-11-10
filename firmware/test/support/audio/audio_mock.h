#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Управление мок-кодеком host-стенда T121 П5 (реализация — audio_mock.c).
 *
 * Мок стоит на месте esp_audio_play: запоминает размер и первые сэмплы
 * каждого запрошенного блока. Этого хватает, чтобы стенд видел настоящую
 * очередь AudioOut и настоящий порядок воспроизведения, не поднимая I2S.
 */
typedef struct {
    int bytes;        /* сколько байт просили сыграть */
    int16_t first[2]; /* первые два сэмпла блока (L/R) */
    bool silence;     /* правда, если первые 640 байт — нули */
} audio_mock_call_t;

/** Сколько раз просили проиграть блок. */
int audio_mock_play_calls(void);

/** Суммарный объём всех запрошенных блоков, байты. */
int audio_mock_total_bytes(void);

/** Описание i-го вызова; NULL — такого вызова не было. */
const audio_mock_call_t *audio_mock_call(int index);

/** Задержка «кодека» на каждый блок: имитирует блокирующее воспроизведение. */
void audio_mock_set_delay_us(int delay_us);

/** Заставить N-й (с 1) вызов esp_audio_play вернуть ошибку; 0 — сбросить. */
void audio_mock_fail_at(int call_number, int code);

/** Забыть записанные вызовы (задержку и настройку ошибки не трогает). */
void audio_mock_reset(void);

#ifdef __cplusplus
}
#endif
