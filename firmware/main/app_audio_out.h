/*
 * Единая синхронизация TX аудиокодека (ES8311 / esp_audio_play, esp_audio_set_play_vol).
 *
 * Вендорный bsp_audio_play / esp_codec_dev_write не содержит собственного мьютекса,
 * поэтому конкурентная запись из fsm.speech (озвучивание ответа) и record_task /
 * fsm.send (звуковые тоны KOUKI_TONE_*) повреждает I2S TX поток или вызывает ошибку
 * записи. Модуль задаёт единый leaf-lock:
 *   VoiceStateManager::m_lock (Level 1) -> kouki_audio_out_lock() (Level 2).
 * Код внутри критической секции audio_out НИКОГДА не должен брать VoiceStateManager::m_lock
 * или app_wake::s_wake_mutex.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Инициализирует мьютекс TX кодека при запуске системы. */
esp_err_t kouki_audio_out_init(void);

/** @brief Захватывает монопольный доступ к TX кодеку. */
void kouki_audio_out_lock(void);

/** @brief Освобождает доступ к TX кодеку. */
void kouki_audio_out_unlock(void);

#ifdef __cplusplus
}
#endif
