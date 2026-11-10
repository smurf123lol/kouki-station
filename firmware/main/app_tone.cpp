#include "app_tone.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app_wake.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if !defined(CONFIG_KOUKI_SOUND_SIGNALS)

/*
 * Звуковые сигналы выключены в menuconfig: API остаётся, но код синтеза
 * и зависимости от аудио-борды не компилируются вовсе.
 */
namespace kouki {
namespace {

const char *const TAG = "kouki.tone";

}  // namespace

Tone::Tone(AudioOut &audio) : m_audio(audio) {}

void Tone::play(kouki_tone_t tone)
{
    (void)tone;
    ESP_LOGD(TAG, "звуковые сигналы выключены (CONFIG_KOUKI_SOUND_SIGNALS)");
}

Tone &tone()
{
    static Tone s_instance(audio_out());
    return s_instance;
}

}  // namespace kouki

extern "C" void kouki_tone_play(kouki_tone_t tone)
{
    kouki::tone().play(tone);
}

#else

namespace kouki {
namespace {

const char *const TAG = "kouki.tone";

constexpr uint32_t TONE_SAMPLE_RATE = 16000;
constexpr uint32_t TONE_FADE_MS = 5;
constexpr size_t TONE_FADE_SAMPLES = TONE_SAMPLE_RATE * TONE_FADE_MS / 1000;
constexpr float TONE_PI = 3.14159265358979323846f;

/** Длительность шага в сэмплах при 16 кГц. */
size_t tone_samples(uint16_t duration_ms)
{
    return ((size_t)TONE_SAMPLE_RATE * (size_t)duration_ms) / 1000u;
}

}  // namespace

Tone::Tone(AudioOut &audio) : m_audio(audio) {}

esp_err_t Tone::generate_step(const Step &step, int16_t volume_percent, int16_t **out_pcm,
                              size_t *out_samples)
{
    if (out_pcm == nullptr || out_samples == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pcm = nullptr;
    *out_samples = 0;

    const size_t samples = tone_samples(step.duration_ms);
    if (samples == 0) {
        return ESP_OK;
    }

    /* Буфер сразу стерео: правый канал расстроен относительно левого. */
    int16_t *pcm = (int16_t *)calloc(samples * 2, sizeof(int16_t));
    if (pcm == nullptr) {
        ESP_LOGE(TAG, "нет памяти под тон: %u сэмплов", (unsigned)samples);
        return ESP_ERR_NO_MEM;
    }

    int32_t volume = volume_percent;
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    const float amplitude = (float)((32767 * volume) / 100);

    /* Тишина (freq 0) уже нулевая после calloc: fade ей не нужен. */
    if (step.freq_hz != 0 && amplitude > 0.0f) {
        size_t fade = TONE_FADE_SAMPLES;
        if (fade * 2 > samples) {
            fade = samples / 2;
        }

        /* Плавный перепад: частота едет от freq_hz к end_hz линейно по шагу. */
        const float f_start = (float)step.freq_hz;
        const float f_end = (float)(step.end_hz != 0 ? step.end_hz : step.freq_hz);
        const float detune = (float)step.detune_x1000 / 1000.0f;
        float phase_l = 0.0f;
        float phase_r = 0.0f;

        for (size_t i = 0; i < samples; i++) {
            float gain = 1.0f;
            if (i < fade) {
                /* Первый сэмпл — ровно ноль: щелчка на старте нет. */
                gain = (float)i / (float)fade;
            } else if (samples - 1 - i < fade) {
                /* Последний сэмпл — тоже ноль: щелчка на выходе нет. */
                gain = (float)(samples - 1 - i) / (float)fade;
            }

            const float t = (float)i / (float)(samples > 1 ? samples - 1 : 1);
            const float freq = f_start + (f_end - f_start) * t;

            if (step.decay_x10 > 0) {
                /* «Капля»: основная энергия в первые 20–30 мс, дальше хвост гаснет. */
                gain *= expf(-0.1f * (float)step.decay_x10 * t);
            }

            const float left = sinf(phase_l) * amplitude * gain;
            const float right = sinf(phase_r) * amplitude * gain;

            phase_l += 2.0f * TONE_PI * freq / (float)TONE_SAMPLE_RATE;
            phase_r += 2.0f * TONE_PI * freq * (1.0f + detune) / (float)TONE_SAMPLE_RATE;
            if (phase_l > 2.0f * TONE_PI) {
                phase_l -= 2.0f * TONE_PI;
            }
            if (phase_r > 2.0f * TONE_PI) {
                phase_r -= 2.0f * TONE_PI;
            }

            pcm[i * 2] =
                (int16_t)(left > 32767.0f ? 32767.0f : (left < -32768.0f ? -32768.0f : left));
            pcm[i * 2 + 1] =
                (int16_t)(right > 32767.0f ? 32767.0f : (right < -32768.0f ? -32768.0f : right));
        }
    }

    *out_pcm = pcm;
    *out_samples = samples;
    return ESP_OK;
}

esp_err_t Tone::synthesize_pattern(const Step *steps, size_t count, int16_t volume_percent,
                                   int16_t **out_pcm, size_t *out_frames)
{
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += tone_samples(steps[i].duration_ms);
    }

    int16_t *pcm = (int16_t *)calloc(total * 2, sizeof(int16_t));
    if (pcm == nullptr) {
        ESP_LOGE(TAG, "нет памяти под сигнал: %u кадров", (unsigned)total);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        int16_t *part = nullptr;
        size_t part_samples = 0;

        const esp_err_t err = generate_step(steps[i], volume_percent, &part, &part_samples);
        if (err != ESP_OK) {
            free(part);
            free(pcm);
            return err;
        }

        if (part_samples > total - offset) {
            free(part);
            free(pcm);
            return ESP_ERR_INVALID_STATE;
        }

        memcpy(&pcm[offset * 2], part, part_samples * 2 * sizeof(int16_t));
        offset += part_samples;
        free(part);
    }

    *out_pcm = pcm;
    *out_frames = total;
    return ESP_OK;
}

void Tone::play(kouki_tone_t tone)
{
    /*
     * Паттерны прежние (T84 §2.2). Таблица вместо switch — те же шаги и тот
     * же порядок; неизвестный сигнал по-прежнему только пишет в лог.
     */
    static const Step kPatternStart[] = {
        {600, 1200, 100, 80, 6},
    };
    static const Step kPatternStopSending[] = {
        {880, 440, 120, 120, 2},
    };
    static const Step kPatternError[] = {
        {300, 200, 150, 50, 12},
    };
    static const Step kPatternWake[] = {
        {500, 900, 50, 150, 8},
        {0, 0, 40, 0, 0},
        {900, 1400, 50, 150, 8},
    };

    const Step *steps = nullptr;
    size_t steps_count = 0;

    switch (tone) {
    case KOUKI_TONE_START:
        steps = kPatternStart;
        steps_count = sizeof(kPatternStart) / sizeof(kPatternStart[0]);
        break;
    case KOUKI_TONE_STOP_SENDING:
        steps = kPatternStopSending;
        steps_count = sizeof(kPatternStopSending) / sizeof(kPatternStopSending[0]);
        break;
    case KOUKI_TONE_ERROR:
        steps = kPatternError;
        steps_count = sizeof(kPatternError) / sizeof(kPatternError[0]);
        break;
    case KOUKI_TONE_WAKE:
        steps = kPatternWake;
        steps_count = sizeof(kPatternWake) / sizeof(kPatternWake[0]);
        break;
    default:
        ESP_LOGW(TAG, "неизвестный сигнал: %d", (int)tone);
        return;
    }

    /* Громкость 0 — осознанная тишина: не занимаем кодек и время вызывающего. */
    const int volume = CONFIG_KOUKI_SIGNAL_VOLUME;
    if (volume <= 0) {
        ESP_LOGD(TAG, "CONFIG_KOUKI_SIGNAL_VOLUME = 0, тон пропущен");
        return;
    }

    int16_t *pcm = nullptr;
    size_t frames = 0;
    const esp_err_t synth_err =
        synthesize_pattern(steps, steps_count, (int16_t)volume, &pcm, &frames);
    if (synth_err != ESP_OK || pcm == nullptr || frames == 0) {
        free(pcm);
        return;
    }

    ESP_LOGD(TAG, "сигнал %d: %u кадров (%.0f мс)", (int)tone, (unsigned)frames,
             1000.0f * (float)frames / (float)TONE_SAMPLE_RATE);

    /*
     * length в esp_audio_play — БАЙТЫ. Буфер уже стерео (L=R), поэтому
     * frames * 2 канала * sizeof(int16_t). bsp_audio_play при 16-битном
     * режиме разворачивает каждый int16 в int32 и сам удваивает length,
     * ожидая на входе чередующиеся L/R (bsp_board.c:359-416).
     * Воспроизведение через AudioOut::play_with_tail под leaf-lock с
     * гарантированной продувкой DMA тишиной (80 мс), исключающей повторы тона
     * в кольцевом буфере.
     */
    const int bytes = (int)(frames * 2u * sizeof(int16_t));
    const esp_err_t play_err = m_audio.play_with_tail(pcm, bytes, 80, portMAX_DELAY);

    /* esp_audio_play пишет в кодек блокирующе — буферу он больше не нужен. */
    free(pcm);

    if (play_err != ESP_OK) {
        ESP_LOGE(TAG, "кодек не принял сигнал: %s", esp_err_to_name(play_err));
    }

    /* После любого звукового сигнала подавляем детектор на 250 мс для защиты от эха */
    kouki_wake_mute(250);
}

Tone &tone()
{
    static Tone s_instance(audio_out());
    return s_instance;
}

}  // namespace kouki

extern "C" void kouki_tone_play(kouki_tone_t tone)
{
    kouki::tone().play(tone);
}

#endif  // CONFIG_KOUKI_SOUND_SIGNALS
