/*
 * Единая синхронизация и диспетчеризация TX аудиокодека (ES8311 / esp_audio_play).
 *
 * Организован как конвейер с очередью FreeRTOS, фоновым worker'ом и поддержкой
 * мгновенных прерываний (Abort).
 *
 * Leaf-lock иерархия:
 *   VoiceStateManager::m_lock (Level 1) -> kouki_audio_out_lock() (Level 2).
 * Внутри critical section audio_out НИКОГДА не берутся мьютексы FSM или wake-word.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#else
#ifndef TickType_t
typedef uint32_t TickType_t;
#endif
#ifndef portMAX_DELAY
#define portMAX_DELAY (0xffffffffUL)
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Инициализирует мьютекс, очередь и воркер вывода аудио. */
esp_err_t kouki_audio_out_init(void);

/** @brief Захватывает монопольный доступ к TX кодеку (leaf-lock). */
void kouki_audio_out_lock(void);

/** @brief Освобождает доступ к TX кодеку. */
void kouki_audio_out_unlock(void);

/**
 * @brief Поставить PCM16 стерео-сэмплы в очередь вывода.
 *
 * @param data Указатель на PCM16 сэмплы.
 * @param bytes Длина данных в байтах.
 * @param transfer_ownership Если true, память data будет автоматически освобождена (free) после проигрывания.
 * @return ESP_OK при успешной постановке в очередь.
 */
esp_err_t kouki_audio_out_enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership);

/**
 * @brief Поставить интервал тишины заданной длительности в очередь.
 *        Гарантирует продувку DMA нулями и естественные паузы речи.
 *
 * @param silence_ms Длительность тишины в миллисекундах.
 * @return ESP_OK при успешной постановке в очередь.
 */
esp_err_t kouki_audio_out_enqueue_silence(uint32_t silence_ms);

/**
 * @brief Мгновенно прерывает текущее воспроизведение и сбрасывает все ожидающие элементы очереди.
 *        Прокачивает короткую тишину для очистки DMA кольца.
 */
void kouki_audio_out_abort(void);

/**
 * @brief Ожидание завершения воспроизведения всех элементов очереди.
 *
 * @param ticks_to_wait Максимальное время ожидания.
 * @return ESP_OK если очередь опустела и кодек замолчал, ESP_ERR_TIMEOUT при истечении таймаута.
 */
esp_err_t kouki_audio_out_wait_idle(TickType_t ticks_to_wait);

/**
 * @brief Проверяет, занят ли аудиовывод (в очереди есть элементы или кодек играет).
 */
bool kouki_audio_out_is_busy(void);

/**
 * @brief Прямое синхронное воспроизведение под leaf-lock (обратная совместимость).
 */
esp_err_t kouki_audio_play(const int16_t *data, int bytes, TickType_t ticks_to_wait);

/**
 * @brief Прямая синхронная продувка DMA тишиной под leaf-lock.
 */
esp_err_t kouki_audio_play_silence(uint32_t silence_ms, TickType_t ticks_to_wait);

/**
 * @brief Синхронное воспроизведение с гарантированным завершающим хвостом тишины.
 */
esp_err_t kouki_audio_play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms, TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
