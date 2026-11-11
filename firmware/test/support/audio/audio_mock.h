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
#define AUDIO_MOCK_EDGE_SAMPLES 200 /* сколько крайних сэмплов запоминаем */

typedef struct {
    int bytes;        /* сколько байт просили сыграть */
    int16_t first[2]; /* первые два сэмпла блока (L/R) */
    bool silence;     /* правда, если первые 640 байт — нули */
    /* Края блока: по ним видно плавный перепад (fade) в начале и в конце.
     * Без них мутант TONE_FADE_MS 5->0 не ловится: первый сэмпл у синуса
     * равен нулю и без fade (sin(0) = 0), а середина блока не видна. */
    int16_t head[AUDIO_MOCK_EDGE_SAMPLES];
    int16_t tail[AUDIO_MOCK_EDGE_SAMPLES];
    int16_t peak;          /* максимальный |сэмпл| по всему блоку */
    int edge_samples;      /* сколько сэмплов реально записано в head/tail */
    int head_frames;       /* сколько кадров (L/R пар) в head */
    int tail_frames;       /* сколько кадров в tail */
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
