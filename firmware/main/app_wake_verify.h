#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Состояние ASR-подтверждения кодового слова.
 *
 * Локальный детектор иногда срабатывает на громкую постороннюю речь (телевизор,
 * разговоры рядом). В домашнем сервере есть ASR: после локального хита станция
 * отправляет первый кусок записи на /api/transcribe и сверяет распознанный текст
 * с «эй гин». REJECTED — хит ложный, запись тихо отменяется.
 */
typedef enum {
    KOUKI_WAKE_VERIFY_IDLE = 0,        /* проверка не запущена */
    KOUKI_WAKE_VERIFY_PENDING = 1,     /* запрос в полёте */
    KOUKI_WAKE_VERIFY_ACCEPTED = 2,    /* текст похож на «эй гин» */
    KOUKI_WAKE_VERIFY_REJECTED = 3,    /* текст не похож — хит ложный */
    KOUKI_WAKE_VERIFY_UNAVAILABLE = 4, /* сервер недоступен/ошибка — доверяем локальному хиту */
} kouki_wake_verify_state_t;

/**
 * @brief Запускает проверку куска PCM (16 кГц, моно, int16) на сервере.
 *
 * Копирует данные во внутренний PSRAM-буфер и создаёт задачу, которая сделает
 * POST /api/transcribe. Не блокирует вызывающего. Если предыдущая проверка ещё
 * идёт, новый запуск игнорируется (ESP_ERR_INVALID_STATE).
 */
esp_err_t kouki_wake_verify_start(const int16_t *pcm, int samples);

/** @brief Текущее состояние проверки (атомарно читается из FSM). */
kouki_wake_verify_state_t kouki_wake_verify_state(void);

/** @brief Сбрасывает состояние в IDLE (новая запись). */
void kouki_wake_verify_reset(void);

/** @brief Последний распознанный текст (для лога; пустая строка, если не было). */
const char *kouki_wake_verify_last_transcript(void);

/** @brief Проверяет текст на «эй гин» (выделено для тестов). */
bool kouki_wake_verify_matches(const char *transcript);

#ifdef __cplusplus
}
#endif
