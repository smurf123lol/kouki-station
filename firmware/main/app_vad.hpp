#ifndef APP_VAD_HPP
#define APP_VAD_HPP

#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: типы и хост-мок остаются общими для C- и
// C++-модулей.
#include "app_vad.h"

#ifdef ESP_PLATFORM
#include "esp_vad.h"
#endif

namespace kouki {

/**
 * Детектор речевой активности (T121 П6).
 *
 * Владеет состоянием модуля: дескриптором WebRTC VAD (только на плате),
 * FIFO остатка сэмплов между вызовами, счётчиком последовательных фреймов речи
 * и признаком последнего фрейма. Экземпляр один (см. kouki::vad()), init()
 * зовётся из app_voice при старте записи; деструктора нет — VAD живёт до
 * перезагрузки.
 */
class Vad {
public:
    /** Размер фрейма VAD: 30 мс при 16 кГц. */
    static constexpr size_t kFrameSamples = 480;

    Vad() = default;
    Vad(const Vad &) = delete;
    Vad &operator=(const Vad &) = delete;

    /** Создаёт WebRTC VAD (VAD_MODE_3) и сбрасывает накопленное состояние. */
    esp_err_t init();

    /** Освобождает дескриптор VAD и обнуляет состояние. */
    void deinit();

    /** Сбрасывает FIFO, streak и триггер детектора. */
    void reset();

    /** Активен ли модуль (был успешный init и не было deinit). */
    bool is_active();

    /**
     * Подаёт PCM 16 кГц/16 бит моно произвольного размера и режет его на
     * фреймы по 30 мс; остаток копится во внутреннем FIFO.
     *
     * @return true, если в чанке была речь (или streak уже больше нуля).
     */
    bool process(const int16_t *pcm, size_t samples);

    /** Число последовательных фреймов речи подряд. */
    uint32_t speech_streak();

    /** true, если streak >= min_frames. */
    bool is_speech_streak(uint32_t min_frames);

#ifndef ESP_PLATFORM
    /** Мок для хост-стендов: принудительный ответ детектора. */
    void host_set_mock(bool enabled, bool speech_result);
#endif

private:
#ifndef ESP_PLATFORM
    /** Энергетическая эмуляция детектора, когда мок выключен. */
    bool host_evaluate(const int16_t *pcm, size_t samples);
#endif

    bool m_initialized = false;
    int16_t m_fifo[kFrameSamples] = {};
    size_t m_fifo_len = 0;
    uint32_t m_speech_streak = 0;
    bool m_last_frame_speech = false;

#ifdef ESP_PLATFORM
    vad_handle_t m_handle = nullptr;
#else
    bool m_host_mock_enabled = false;
    bool m_host_mock_speech = false;
#endif
};

/** Единственный экземпляр детектора. */
Vad &vad();

}  // namespace kouki

#endif  // APP_VAD_HPP
