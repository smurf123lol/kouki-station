#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализирует модуль WebRTC VAD (режим VAD_MODE_3 на ESP32-S3).
 *
 * @return ESP_OK при успешной инициализации, код ошибки иначе.
 */
esp_err_t kouki_vad_init(void);

/**
 * @brief Освобождает ресурсы VAD.
 */
void kouki_vad_deinit(void);

/**
 * @brief Подает PCM аудиопоток (16 кГц, 16 бит моно) произвольного размера
 * и анализирует активность человеческой речи по фреймам 30 мс (480 сэмплов).
 *
 * Остаток сэмплов между вызовами сохраняется во внутреннем FIFO буфере.
 *
 * @param pcm Указатель на массив 16-битных сэмплов PCM 16000 Гц.
 * @param samples Количество сэмплов.
 * @return true если в переданном чанке зафиксирована речь человека.
 */
bool kouki_vad_process(const int16_t *pcm, size_t samples);

/**
 * @brief Возвращает текущее число последовательных 30 мс фреймов речи VAD.
 *
 * Сбрасывается в 0 при обнаружении фрейма тишины/шума или при вызове kouki_vad_reset().
 */
uint32_t kouki_vad_get_speech_streak(void);

/**
 * @brief Проверяет, зафиксирована ли серия из не менее min_frames фреймов речи подряд.
 *
 * @param min_frames Минимальное число фреймов (например 3 фрейма = 90 мс).
 * @return true если streak >= min_frames.
 */
bool kouki_vad_is_speech_streak(uint32_t min_frames);

/**
 * @brief Сбрасывает накопленный остаток FIFO, счетчик streak и триггер детектора.
 */
void kouki_vad_reset(void);

/**
 * @brief Проверяет, активен ли модуль VAD.
 */
bool kouki_vad_is_active(void);

#ifndef ESP_PLATFORM
/**
 * @brief Управление моком для хост-тестов (принудительный ответ VAD).
 *
 * @param enabled Включен ли мок (если false, работает спектрально-энергетическая эмуляция).
 * @param speech_result Возвращаемый результат детекции речи.
 */
void kouki_vad_host_set_mock(bool enabled, bool speech_result);
#endif

#ifdef __cplusplus
}
#endif
