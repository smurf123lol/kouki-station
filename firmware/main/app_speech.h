/*
 * Озвучивание ответа станции.
 *
 * Сервер уже умеет синтез: POST /api/speech с {"text": "..."} отдаёт WAV
 * 16 кГц моно PCM16. Станция ничего не синтезирует сама — она забирает
 * готовый WAV и играет его через тот же кодек, что и звуковые сигналы.
 *
 * Почему отдельный модуль, а не ответ в /api/voice: голос не нужен, когда
 * станции нечего сказать (ошибка, «Задача записана»), и он не должен
 * задерживать ответ экрана.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Сколько символов ответа озвучиваем: сервер режет ответ по KotaePrompt.MaxAnswerChars. */
#define KOUKI_SPEECH_MAX_CHARS 480

/*
 * Синтезирует и проигрывает текст. Блокирует вызывающего до конца звучания,
 * поэтому звать из задачи, которой не жалко времени (fsm.speech станции).
 *
 * Пустой текст, выключенный Kconfig или ошибка сети — не ошибка вызова:
 * ответ уже показан на экране, звук просто не появится. Причина уходит в лог.
 */
esp_err_t kouki_speech_play(const char *text);

/*
 * Резервирует поколение речи синхронно до создания фоновой задачи, чтобы
 * kouki_speech_abort() отменял запуск ещё до старта kouki_speech_play_gen().
 */
uint32_t kouki_speech_reserve(void);

/* Вариант kouki_speech_play с заранее зарезервированным поколением (0 — выделить внутри). */
esp_err_t kouki_speech_play_gen(const char *text, uint32_t gen);

/* Занята ли станция процессом озвучивания (HTTP-синтез, чанки или хвост DMA) — откладывает запись и OTA. */
bool kouki_speech_busy(void);

/* Активен ли фоновый воркер речи (предотвращает запуск OTA во время очистки предыдущей фразы). */
bool kouki_speech_worker_active(void);

/* Проверяет, активно ли воспроизводится PCM-аудио в кодек прямо сейчас (для пульсации UI). */
bool kouki_speech_is_playing(void);

/* Прерывает текущее воспроизведение речи (отменяет следующие фразы). */
void kouki_speech_abort(void);

/*
 * Разбор WAV и разворот моно в стерео — отдельно от HTTP и кодека,
 * чтобы их можно было проверить на хосте без ESP-IDF (см. host-стенд).
 */
typedef struct {
    size_t data_offset;      /* где начинаются сэмплы */
    size_t data_bytes;       /* сколько сэмплов пришло (уже обрезано по длине файла) */
    uint16_t channels;       /* 1 или 2 */
    uint32_t sample_rate;    /* ожидаем 16000 */
    uint16_t bits_per_sample;/* ожидаем 16 */
} kouki_speech_wav_t;

/* Проверяет заголовок и находит чанк data; принимает только PCM16 моно/стерео. */
esp_err_t kouki_speech_wav_parse(const uint8_t *wav, size_t len, kouki_speech_wav_t *out);

/*
 * Разворачивает моно в стереопару L=R на месте, с конца.
 * Буфер должен вмещать 2 * frames сэмплов.
 */
esp_err_t kouki_speech_stereo_expand(int16_t *samples, size_t frames);

#ifdef __cplusplus
}
#endif
