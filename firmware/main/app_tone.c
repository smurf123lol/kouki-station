/*
 * Звуковые сигналы станции: синтез PCM16 16 кГц и вывод через аудиокодек.
 *
 * Воспроизведение открыто в стерео (main.c: esp_board_init(16000, 2, 16)),
 * поэтому моно-сэмпл на отправке разворачивается в пару L=R: иначе
 * bsp_audio_play трактует входной поток как чередующиеся каналы, и тон
 * звучит вдвое короче и вдвое выше задуманного.
 */
#include "app_tone.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

#if !defined(CONFIG_KOUKI_SOUND_SIGNALS)
/*
 * Звуковые сигналы выключены в menuconfig: API остаётся, но код синтеза
 * и зависимости от аудио-борды не компилируются вовсе.
 */
static const char *TAG = "kouki.tone";

void kouki_tone_play(kouki_tone_t tone)
{
    (void)tone;
    ESP_LOGD(TAG, "звуковые сигналы выключены (CONFIG_KOUKI_SOUND_SIGNALS)");
}

#else

#include <math.h>

#include "esp_board_init.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "kouki.tone";

#define TONE_SAMPLE_RATE   16000

/* Хвост тишины после сигнала: 0,25 с стереопары при 16 кГц. */
#define TONE_TAIL_FRAMES 4000u
static int16_t s_tone_tail[TONE_TAIL_FRAMES * 2u];
/* Расстройка правого канала: 0,4 % по высоте дают «шершавость» на слух. */
#define TONE_DETUNE        0.004f
#define TONE_FADE_MS       5
#define TONE_FADE_SAMPLES  (TONE_SAMPLE_RATE * TONE_FADE_MS / 1000)
#define TONE_PI            3.14159265358979323846f

/* Шаг паттерна: частота и длительность. freq_hz == 0 — пауза (тишина). */
typedef struct {
    uint16_t freq_hz;       /* с какой частоты начинается шаг */
    uint16_t end_hz;        /* куда приходит; 0 — держим freq_hz */
    uint16_t duration_ms;
    bool decay;              /* true — «капля»: короткий спад после удара */
} tone_step_t;

/*
 * Звучание: «капля» — короткий подъём высоты с быстрым спадом громкости,
 * как капля изо рта. Частоты разные, чтобы сигналы различались на слух:
 * старт записи — выше, отправка — ниже, ошибка — совсем низкая,
 * кодовое слово — самый широкий размах и самая звонкая.
 */
static const tone_step_t PATTERN_START[] = {
    {500, 1500, 110, true},
};

static const tone_step_t PATTERN_STOP_SENDING[] = {
    {380, 900, 120, true},
};

static const tone_step_t PATTERN_ERROR[] = {
    {260, 520, 170, true},
};

static const tone_step_t PATTERN_WAKE[] = {
    {600, 1600, 100, true},
};

static size_t tone_samples(uint16_t duration_ms)
{
    return ((size_t)TONE_SAMPLE_RATE * (size_t)duration_ms) / 1000u;
}

/*
 * Синтез одного шага: синус 16 кГц PCM16 моно с fade-in/fade-out по 5 мс.
 * Буфер выделяется в куче и освобождается вызывающим после отправки в кодек.
 */
static esp_err_t generate_tone(uint16_t freq_hz, uint16_t end_hz, uint16_t duration_ms,
                               bool decay, int16_t volume_percent, int16_t **out_pcm,
                               size_t *out_samples)
{
    if (out_pcm == NULL || out_samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pcm = NULL;
    *out_samples = 0;

    const size_t samples = tone_samples(duration_ms);
    if (samples == 0) {
        return ESP_OK;
    }

    /* Буфер сразу стерео: левый и правый каналы расстроены на TONE_DETUNE. */
    int16_t *pcm = calloc(samples * 2, sizeof(int16_t));
    if (pcm == NULL) {
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
    if (freq_hz != 0 && amplitude > 0.0f) {
        size_t fade = TONE_FADE_SAMPLES;
        if (fade * 2 > samples) {
            fade = samples / 2;
        }

        /* Плавный перепад: частота едет от freq_hz к end_hz линейно по шагу. */
        const float f_start = (float)freq_hz;
        const float f_end = (float)(end_hz != 0 ? end_hz : freq_hz);
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

            if (decay) {
                /* «Капля»: основная энергия в первые 20–30 мс, дальше хвост гаснет. */
                gain *= expf(-4.5f * t);
            }

            const float left = sinf(phase_l) * amplitude * gain;
            const float right = sinf(phase_r) * amplitude * gain;

            phase_l += 2.0f * TONE_PI * freq / (float)TONE_SAMPLE_RATE;
            phase_r += 2.0f * TONE_PI * freq * (1.0f + TONE_DETUNE) / (float)TONE_SAMPLE_RATE;
            if (phase_l > 2.0f * TONE_PI) {
                phase_l -= 2.0f * TONE_PI;
            }
            if (phase_r > 2.0f * TONE_PI) {
                phase_r -= 2.0f * TONE_PI;
            }

            pcm[i * 2] = (int16_t)(left > 32767.0f ? 32767.0f : (left < -32768.0f ? -32768.0f : left));
            pcm[i * 2 + 1] = (int16_t)(right > 32767.0f ? 32767.0f : (right < -32768.0f ? -32768.0f : right));
        }
    }

    *out_pcm = pcm;
    *out_samples = samples;
    return ESP_OK;
}

/*
 * Паттерн целиком: тоны и паузы в одном буфере, чтобы отдать кодеку один
 * блок и не ловить щелчки на стыках между отдельными вызовами.
 *
 * Каждый шаг синтезируется сразу в стерео: правый канал расстроен, поэтому
 * кадры не совпадают по фазе и звук «шершавый». out_frames — число кадров
 * стерео; байт в буфере вдвое больше.
 */
static esp_err_t synthesize_pattern(const tone_step_t *steps, size_t count,
                                    int16_t volume_percent, int16_t **out_pcm,
                                    size_t *out_frames)
{
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += tone_samples(steps[i].duration_ms);
    }

    int16_t *pcm = calloc(total * 2, sizeof(int16_t));
    if (pcm == NULL) {
        ESP_LOGE(TAG, "нет памяти под сигнал: %u кадров", (unsigned)total);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    for (size_t i = 0; i < count; i++) {
        int16_t *part = NULL;
        size_t part_samples = 0;

        const esp_err_t err = generate_tone(steps[i].freq_hz, steps[i].end_hz,
                                            steps[i].duration_ms, steps[i].decay,
                                            volume_percent, &part, &part_samples);
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

void kouki_tone_play(kouki_tone_t tone)
{
    const tone_step_t *steps = NULL;
    size_t steps_count = 0;

    switch (tone) {
    case KOUKI_TONE_START:
        steps = PATTERN_START;
        steps_count = sizeof(PATTERN_START) / sizeof(PATTERN_START[0]);
        break;
    case KOUKI_TONE_STOP_SENDING:
        steps = PATTERN_STOP_SENDING;
        steps_count = sizeof(PATTERN_STOP_SENDING) / sizeof(PATTERN_STOP_SENDING[0]);
        break;
    case KOUKI_TONE_ERROR:
        steps = PATTERN_ERROR;
        steps_count = sizeof(PATTERN_ERROR) / sizeof(PATTERN_ERROR[0]);
        break;
    case KOUKI_TONE_WAKE:
        steps = PATTERN_WAKE;
        steps_count = sizeof(PATTERN_WAKE) / sizeof(PATTERN_WAKE[0]);
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

    int16_t *pcm = NULL;
    size_t frames = 0;
    const esp_err_t synth_err = synthesize_pattern(steps, steps_count, (int16_t)volume,
                                                   &pcm, &frames);
    if (synth_err != ESP_OK || pcm == NULL || frames == 0) {
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
     */
    const int bytes = (int)(frames * 2u * sizeof(int16_t));
    const esp_err_t play_err = esp_audio_play(pcm, bytes, portMAX_DELAY);

    /*
     * Хвост тишины: DMA проигрывает последний буфер ещё раз, и короткий тон
     * без паузы зацикливается (слышно как «дррр» вместо «пип»). 0,25 с
     * стереопары = 4000 кадров; буфер статический и нулевой.
     */
    if (play_err == ESP_OK) {
        esp_audio_play(s_tone_tail, (int)sizeof(s_tone_tail), portMAX_DELAY);
    }

    /* esp_audio_play пишет в кодек блокирующе — буферу он больше не нужен. */
    free(pcm);

    if (play_err != ESP_OK) {
        ESP_LOGE(TAG, "кодек не принял сигнал: %s", esp_err_to_name(play_err));
    }
}

#endif // CONFIG_KOUKI_SOUND_SIGNALS
