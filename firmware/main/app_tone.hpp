#ifndef APP_TONE_HPP
#define APP_TONE_HPP

#include <stddef.h>
#include <stdint.h>

// C-совместимый заголовок фасада: тип kouki_tone_t остаётся общим для
// C- и C++-модулей (его используют FSM-состояния и app_voice.cpp).
#include "app_tone.h"

#include "app_audio_out.hpp"

namespace kouki {

/**
 * Звуковые сигналы станции (T121 П5).
 *
 * Синтезирует PCM16 16 кГц процедурно и проигрывает через AudioOut, который
 * приходит ссылкой в конструктор: своего доступа к кодеку и второй задачи у
 * Tone нет — I2S остаётся с одним владельцем (AudioOut).
 *
 * Шаги паттерна неизменяемы и лежат константами; состояние у класса
 * отсутствует (буфер синтеза живёт на стеке вызова), поэтому экземпляр один
 * и создаётся фабрикой kouki::tone().
 */
class Tone {
public:
    explicit Tone(AudioOut &audio);

    Tone(const Tone &) = delete;
    Tone &operator=(const Tone &) = delete;

    /**
     * Синтезирует и проигрывает сигнал (блокирующий вызов, как в C-модуле).
     *
     * При CONFIG_KOUKI_SOUND_SIGNALS=n ничего не играет; при громкости
     * CONFIG_KOUKI_SIGNAL_VOLUME=0 — тишина без обращения к кодеку.
     */
    void play(kouki_tone_t tone);

private:
    /** Шаг паттерна: частота и длительность; freq_hz == 0 — пауза. */
    struct Step {
        uint16_t freq_hz;
        uint16_t end_hz;
        uint16_t duration_ms;
        uint16_t decay_x10;
        uint16_t detune_x1000;
    };

    /**
     * Собирает паттерн целиком в один стерео-буфер.
     *
     * Один блок отдаётся кодеку целиком, чтобы не ловить щелчки на стыках
     * между отдельными вызовами.
     */
    esp_err_t synthesize_pattern(const Step *steps, size_t count, int16_t volume_percent,
                                 int16_t **out_pcm, size_t *out_frames);

    /**
     * Синтез одного шага: синус 16 кГц PCM16 с fade-in/fade-out по 5 мс.
     * Буфер выделяется в куче и освобождается вызывающим после отправки.
     */
    esp_err_t generate_step(const Step &step, int16_t volume_percent, int16_t **out_pcm,
                            size_t *out_samples);

    AudioOut &m_audio;
};

/** Единственный экземпляр синтезатора сигналов. */
Tone &tone();

}  // namespace kouki

#endif  // APP_TONE_HPP
