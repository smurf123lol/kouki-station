#include "esp_timer.h"
#include "app_wake.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "dl_rfft.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "wake_model_ey_gin.h"

static const char *TAG = "kouki.wake";

#define FFT_POINTS       512
#define FRAME_LEN        480    /* 30 мс при 16 кГц */
#define FRAME_STEP       160    /* 10 мс при 16 кГц */
#define MEL_BINS_COUNT   40
#define TOTAL_CELLS      (WAKE_MODEL_FRAMES * WAKE_MODEL_MELS)

static const int16_t g_mel_bins[MEL_BINS_COUNT + 2] = {
    2, 4, 5, 7, 9, 10, 12, 15, 17, 19, 22, 24, 27, 30, 33, 36, 40, 44,
    48, 52, 56, 61, 66, 71, 77, 83, 89, 96, 103, 110, 118, 127, 135,
    145, 155, 165, 177, 188, 201, 214, 228, 243
};

static bool s_initialized = false;
static dl_fft_f32_t *s_fft = NULL;
static float s_hann_window[FRAME_LEN];
static float s_pcm_window[FRAME_LEN];
static float s_step_buf[FRAME_STEP];
static int s_step_buf_len = 0;
static float *s_spectrogram = NULL;
static float *s_norm_spec = NULL;
static float *s_infer_arena = NULL;
static int s_step_counter = 0;
static int s_frames_collected = 0;
static int16_t s_frame_peak[WAKE_MODEL_FRAMES];
static int16_t s_step_peak = 0;

esp_err_t kouki_wake_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!kouki_config_get_wake_enabled()) {
        ESP_LOGI(TAG, "детектор wake-word выключен в конфигурации NVS (wake_en=0)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_fft = dl_rfft_f32_init(FFT_POINTS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fft) {
        ESP_LOGE(TAG, "не удалось инициализировать FFT-инстанс %d точек", FFT_POINTS);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < FRAME_LEN; i++) {
        s_hann_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(FRAME_LEN - 1)));
    }
    memset(s_pcm_window, 0, sizeof(s_pcm_window));
    s_step_buf_len = 0;

    /* Аллокация буфера спектрограммы (23.6 КБ) в PSRAM или куче */
    s_spectrogram = (float *)heap_caps_malloc(TOTAL_CELLS * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_spectrogram) {
        s_spectrogram = (float *)heap_caps_malloc(TOTAL_CELLS * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_spectrogram) {
        ESP_LOGE(TAG, "не удалось выделить память под буфер спектрограммы (%u байт)", (unsigned)(TOTAL_CELLS * sizeof(float)));
        dl_rfft_f32_deinit(s_fft);
        s_fft = NULL;
        return ESP_ERR_NO_MEM;
    }
    for (int c = 0; c < TOTAL_CELLS; c++) {
        s_spectrogram[c] = -13.81551f;
    }
    memset(s_frame_peak, 0, sizeof(s_frame_peak));
    s_step_peak = 0;

    /* Буфер нормализованной спектрограммы для инференса */
    s_norm_spec = (float *)heap_caps_malloc(TOTAL_CELLS * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_norm_spec) {
        s_norm_spec = (float *)heap_caps_malloc(TOTAL_CELLS * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_norm_spec) {
        free(s_spectrogram);
        s_spectrogram = NULL;
        dl_rfft_f32_deinit(s_fft);
        s_fft = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_norm_spec, 0, TOTAL_CELLS * sizeof(float));

    /* Буфер промежуточных активаций инференса (142 КБ) в PSRAM */
    s_infer_arena = (float *)heap_caps_malloc(WAKE_MODEL_ARENA_FLOATS * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_infer_arena) {
        s_infer_arena = (float *)heap_caps_malloc(WAKE_MODEL_ARENA_FLOATS * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_infer_arena) {
        ESP_LOGE(TAG, "не удалось выделить память под арену активаций (%u байт)", (unsigned)(WAKE_MODEL_ARENA_FLOATS * sizeof(float)));
        free(s_norm_spec);
        s_norm_spec = NULL;
        free(s_spectrogram);
        s_spectrogram = NULL;
        dl_rfft_f32_deinit(s_fft);
        s_fft = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_infer_arena, 0, WAKE_MODEL_ARENA_FLOATS * sizeof(float));

    s_step_counter = 0;
    s_frames_collected = 0;
    s_initialized = true;

    uint8_t th = kouki_config_get_wake_threshold();
    (void)th;
    (void)TAG;
    ESP_LOGI(TAG, "детектор «эй гин» готов: microWakeWord INT8 (%u байт), порог %u%% (%.2f), окно %d с",
             (unsigned)WAKE_MODEL_EY_GIN_LEN, (unsigned)th, (double)th / 100.0, 1);

    return ESP_OK;
}

static int64_t s_mute_until_us = 0;
static int s_consecutive_hits = 0;

void kouki_wake_reset(void)
{
    s_step_buf_len = 0;
    s_step_counter = 0;
    s_consecutive_hits = 0;
    s_step_peak = 0;
    s_frames_collected = 0;
    if (s_spectrogram) {
        for (int c = 0; c < TOTAL_CELLS; c++) {
            s_spectrogram[c] = -13.81551f;
        }
    }
    memset(s_pcm_window, 0, sizeof(s_pcm_window));
    memset(s_frame_peak, 0, sizeof(s_frame_peak));
}

void kouki_wake_mute(int duration_ms)
{
    s_mute_until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
    kouki_wake_reset();
}

void kouki_wake_deinit(void)
{
    if (s_fft) {
        dl_rfft_f32_deinit(s_fft);
        s_fft = NULL;
    }
    if (s_spectrogram) {
        free(s_spectrogram);
        s_spectrogram = NULL;
    }
    if (s_norm_spec) {
        free(s_norm_spec);
        s_norm_spec = NULL;
    }
    if (s_infer_arena) {
        free(s_infer_arena);
        s_infer_arena = NULL;
    }
    s_initialized = false;
    kouki_wake_reset();
}

bool kouki_wake_is_ready(void)
{
    return s_initialized && kouki_config_get_wake_enabled();
}

const char *kouki_wake_model(void)
{
    return "эй гин";
}

static void process_frame(void)
{
    static float fft_buf[FFT_POINTS];
    memset(fft_buf, 0, sizeof(fft_buf));

    for (int i = 0; i < FRAME_LEN; i++) {
        fft_buf[i] = s_pcm_window[i] * s_hann_window[i];
    }

    dl_rfft_f32_run(s_fft, fft_buf);

    /* Вычисление спектра мощности */
    static float power_spec[FFT_POINTS / 2 + 1];
    power_spec[0] = (fft_buf[0] * fft_buf[0]) / (float)FRAME_LEN;
    power_spec[FFT_POINTS / 2] = (fft_buf[1] * fft_buf[1]) / (float)FRAME_LEN;
    for (int k = 1; k < FFT_POINTS / 2; k++) {
        float re = fft_buf[2 * k];
        float im = fft_buf[2 * k + 1];
        power_spec[k] = (re * re + im * im) / (float)FRAME_LEN;
    }

    /* Mel-фильтры */
    float log_mel[MEL_BINS_COUNT];
    for (int m = 0; m < MEL_BINS_COUNT; m++) {
        int left = g_mel_bins[m];
        int center = g_mel_bins[m + 1];
        int right = g_mel_bins[m + 2];
        float energy = 0.0f;

        for (int k = left; k < center; k++) {
            float w = (float)(k - left) / (float)(center - left);
            energy += power_spec[k] * w;
        }
        for (int k = center; k < right; k++) {
            float w = (float)(right - k) / (float)(right - center);
            energy += power_spec[k] * w;
        }
        log_mel[m] = logf(energy + 1e-6f);
    }

    /* Сдвиг матрицы спектрограммы на 1 фрейм влево */
    memmove(s_spectrogram, s_spectrogram + MEL_BINS_COUNT,
            (WAKE_MODEL_FRAMES - 1) * MEL_BINS_COUNT * sizeof(float));
    memcpy(s_spectrogram + (WAKE_MODEL_FRAMES - 1) * MEL_BINS_COUNT,
           log_mel, MEL_BINS_COUNT * sizeof(float));
}

bool kouki_wake_feed(const int16_t *pcm, int samples)
{
    if (!s_initialized || !kouki_config_get_wake_enabled() || !pcm || samples <= 0) {
        return false;
    }

    if (esp_timer_get_time() < s_mute_until_us) {
        return false;
    }

    bool detected = false;
    const float threshold = (float)kouki_config_get_wake_threshold() / 100.0f;

    for (int i = 0; i < samples; i++) {
        int16_t val = pcm[i];
        int16_t abs_val = (val < 0) ? -val : val;
        if (abs_val > s_step_peak) {
            s_step_peak = abs_val;
        }

        s_step_buf[s_step_buf_len++] = (float)val / 32768.0f;

        if (s_step_buf_len >= FRAME_STEP) {
            s_step_buf_len = 0;

            /* Сохраняем пиковую амплитуду за шаг 160 сэмплов */
            memmove(s_frame_peak, s_frame_peak + 1, (WAKE_MODEL_FRAMES - 1) * sizeof(int16_t));
            s_frame_peak[WAKE_MODEL_FRAMES - 1] = s_step_peak;
            s_step_peak = 0;

            /* Блочный сдвиг окна PCM на FRAME_STEP сэмплов */
            memmove(s_pcm_window, s_pcm_window + FRAME_STEP, (FRAME_LEN - FRAME_STEP) * sizeof(float));
            memcpy(s_pcm_window + (FRAME_LEN - FRAME_STEP), s_step_buf, FRAME_STEP * sizeof(float));

            process_frame();
            if (s_frames_collected < WAKE_MODEL_FRAMES) {
                s_frames_collected++;
            }
            s_step_counter++;

            /* Инференс каждые 10 фреймов (каждые 100 мс) для более отзывчивой реакции */
            if (s_step_counter >= 10) {
                s_step_counter = 0;

                /* Не запускаем инференс, пока буфер не прогрелся полным окном (148 фреймов = 1.48 с) */
                if (s_frames_collected < WAKE_MODEL_FRAMES) {
                    s_consecutive_hits = 0;
                    continue;
                }

                /* Энергетический фильтр: отсекает фоновый шум и тишину (пик речи >= 750) */
                int16_t window_max = 0;
                for (int f = 0; f < WAKE_MODEL_FRAMES; f++) {
                    if (s_frame_peak[f] > window_max) {
                        window_max = s_frame_peak[f];
                    }
                }

                if (window_max < 750) {
                    s_consecutive_hits = 0;
                    continue;
                }

                /* Стандартизация спектрограммы (mean 0, std 1) */
                float sum = 0.0f;
                float sq_sum = 0.0f;
                for (int c = 0; c < TOTAL_CELLS; c++) {
                    float v = s_spectrogram[c];
                    sum += v;
                    sq_sum += v * v;
                }
                float mean = sum / (float)TOTAL_CELLS;
                float var = (sq_sum / (float)TOTAL_CELLS) - (mean * mean);
                float std = sqrtf((var > 1e-6f) ? var : 1e-6f);

                for (int c = 0; c < TOTAL_CELLS; c++) {
                    s_norm_spec[c] = (s_spectrogram[c] - mean) / std;
                }

                float prob = wake_model_ey_gin_infer(s_norm_spec, s_infer_arena);

                if (prob >= threshold) {
                    s_consecutive_hits++;
                    if (s_consecutive_hits >= 2) {
                        ESP_LOGI(TAG, "распознано «эй гин»! prob=%.3f >= %.2f (порог NVS)",
                                 (double)prob, (double)threshold);
                        detected = true;
                        s_consecutive_hits = 0;
                        kouki_wake_reset();
                    }
                } else {
                    s_consecutive_hits = 0;
                }
            }
        }
    }

    return detected;
}
