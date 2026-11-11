#include "app_wake.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.hpp"
#include "app_http.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "wake_model_ey_gin.h"

namespace kouki {
namespace {

const char *const TAG = "kouki.wake";

/* Таблицы mel-фильтров модели microWakeWord INT8: края 40 треугольных
 * фильтров в бинах 512-точечного спектра. Константа на весь модуль. */
const int16_t kMelBinEdges[Wake::kMelBins + 2] = {
    2, 4, 5, 7, 9, 10, 12, 15, 17, 19, 22, 24, 27, 30, 33, 36, 40, 44,
    48, 52, 56, 61, 66, 71, 77, 83, 89, 96, 103, 110, 118, 127, 135,
    145, 155, 165, 177, 188, 201, 214, 228, 243
};

/* Сколько ячеек спектрограммы модели (кадры × mel-бины). */
constexpr int kTotalCells = Wake::kModelFrames * Wake::kMelBins;

/* Значение «тишины» в логарифмическом mel-спектре: log(1e-6). */
constexpr float kSilenceMelLog = -13.81551f;

/* Модель и раскладка спектрограммы обязаны совпадать: иначе инференс молча
 * читает не те кадры. Проверяем на этапе компиляции. */
static_assert(Wake::kModelFrames == WAKE_MODEL_FRAMES, "кадров модели не совпало");
static_assert(Wake::kMelBins == WAKE_MODEL_MELS, "mel-бинов модели не совпало");
static_assert(Wake::kFftPoints == 512, "FFT модели рассчитана на 512 точек");

}  // namespace

Wake::Wake(Config &config, Http &http) : m_config(config), m_http(http) {}

bool Wake::lock_state()
{
    if (m_mutex != nullptr) {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
        return true;
    }
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == nullptr) {
        return false;
    }
    taskENTER_CRITICAL(&m_spinlock);
    if (m_mutex == nullptr) {
        m_mutex = created;
        created = nullptr;
    }
    taskEXIT_CRITICAL(&m_spinlock);
    if (created != nullptr) {
        vSemaphoreDelete(created);
    }
    if (m_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    return true;
}

void Wake::unlock_state()
{
    if (m_mutex != nullptr) {
        xSemaphoreGive(m_mutex);
    }
}

void Wake::reset_locked()
{
    m_step_buf_len = 0;
    m_step_counter = 0;
    m_consecutive_hits = 0;
    m_step_peak = 0;
    m_frames_collected = 0;
    if (m_spectrogram) {
        for (int c = 0; c < kTotalCells; c++) {
            m_spectrogram[c] = kSilenceMelLog;
        }
    }
    memset(m_pcm_window, 0, sizeof(m_pcm_window));
    memset(m_frame_peak, 0, sizeof(m_frame_peak));
}

esp_err_t Wake::init()
{
    if (!lock_state()) {
        return ESP_ERR_NO_MEM;
    }

    if (m_initialized) {
        unlock_state();
        return ESP_OK;
    }

    if (!m_config.wake_enabled()) {
        unlock_state();
        ESP_LOGI(TAG, "детектор wake-word выключен в конфигурации NVS (wake_en=0)");
        return ESP_ERR_NOT_SUPPORTED;
    }

    m_fft = dl_rfft_f32_init(kFftPoints, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!m_fft) {
        unlock_state();
        ESP_LOGE(TAG, "не удалось инициализировать FFT-инстанс %d точек", kFftPoints);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < kFrameLen; i++) {
        m_hann_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(kFrameLen - 1)));
    }
    memset(m_pcm_window, 0, sizeof(m_pcm_window));
    m_step_buf_len = 0;

    /* Аллокация буфера спектрограммы (23.6 КБ) в PSRAM или куче */
    m_spectrogram = (float *)heap_caps_malloc(kTotalCells * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_spectrogram) {
        m_spectrogram = (float *)heap_caps_malloc(kTotalCells * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!m_spectrogram) {
        ESP_LOGE(TAG, "не удалось выделить память под буфер спектрограммы (%u байт)", (unsigned)(kTotalCells * sizeof(float)));
        dl_rfft_f32_deinit(m_fft);
        m_fft = nullptr;
        unlock_state();
        return ESP_ERR_NO_MEM;
    }
    for (int c = 0; c < kTotalCells; c++) {
        m_spectrogram[c] = kSilenceMelLog;
    }
    memset(m_frame_peak, 0, sizeof(m_frame_peak));
    m_step_peak = 0;

    /* Буфер нормализованной спектрограммы для инференса */
    m_norm_spec = (float *)heap_caps_malloc(kTotalCells * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_norm_spec) {
        m_norm_spec = (float *)heap_caps_malloc(kTotalCells * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!m_norm_spec) {
        free(m_spectrogram);
        m_spectrogram = nullptr;
        dl_rfft_f32_deinit(m_fft);
        m_fft = nullptr;
        unlock_state();
        return ESP_ERR_NO_MEM;
    }
    memset(m_norm_spec, 0, kTotalCells * sizeof(float));

    /* Буфер промежуточных активаций инференса (142 КБ) в PSRAM */
    m_infer_arena = (float *)heap_caps_malloc(WAKE_MODEL_ARENA_FLOATS * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_infer_arena) {
        m_infer_arena = (float *)heap_caps_malloc(WAKE_MODEL_ARENA_FLOATS * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!m_infer_arena) {
        ESP_LOGE(TAG, "не удалось выделить память под арену активаций (%u байт)", (unsigned)(WAKE_MODEL_ARENA_FLOATS * sizeof(float)));
        free(m_norm_spec);
        m_norm_spec = nullptr;
        free(m_spectrogram);
        m_spectrogram = nullptr;
        dl_rfft_f32_deinit(m_fft);
        m_fft = nullptr;
        unlock_state();
        return ESP_ERR_NO_MEM;
    }
    memset(m_infer_arena, 0, WAKE_MODEL_ARENA_FLOATS * sizeof(float));

    m_step_counter = 0;
    m_frames_collected = 0;
    m_consecutive_hits = 0;
    m_initialized = true;
    m_ready.store(true, std::memory_order_release);

    const uint8_t th = m_config.wake_threshold();
    unlock_state();
    ESP_LOGI(TAG, "детектор «эй гин» готов: microWakeWord INT8 (%u байт), порог %u%% (%.2f), окно %d с",
             (unsigned)WAKE_MODEL_EY_GIN_LEN, (unsigned)th, (double)th / 100.0, 1);

    /* Первые секунды после старта — щелчки включения и DC-переход микрофона;
       на пороге 65 % они дают ложный хит. Глушим детектор, пока тракт не уляжется. */
    mute(2500);

    return ESP_OK;
}

void Wake::deinit()
{
    const bool locked = lock_state();
    m_ready.store(false, std::memory_order_release);
    m_initialized = false;
    if (m_fft) {
        dl_rfft_f32_deinit(m_fft);
        m_fft = nullptr;
    }
    if (m_spectrogram) {
        free(m_spectrogram);
        m_spectrogram = nullptr;
    }
    if (m_norm_spec) {
        free(m_norm_spec);
        m_norm_spec = nullptr;
    }
    if (m_infer_arena) {
        free(m_infer_arena);
        m_infer_arena = nullptr;
    }
    reset_locked();
    if (locked) {
        unlock_state();
    }
}

bool Wake::is_ready()
{
    return m_ready.load(std::memory_order_acquire) && m_config.wake_enabled();
}

const char *Wake::model() const
{
    return "эй гин";
}

void Wake::mute(int duration_ms)
{
    if (duration_ms > 0) {
        const int64_t until_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
        int64_t cur = m_mute_until_us.load(std::memory_order_relaxed);
        while (until_us > cur &&
               !m_mute_until_us.compare_exchange_weak(cur, until_us,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
        }
    }
    reset();
}

void Wake::reset()
{
    const bool locked = lock_state();
    reset_locked();
    if (locked) {
        unlock_state();
    }
}

int Wake::get_preroll(int16_t *dst, int max_samples)
{
    if (!dst || max_samples <= 0) {
        return 0;
    }
    if (!lock_state()) {
        return 0;
    }

    int n = (int)m_preroll_len;
    if (n > max_samples) {
        n = max_samples;
    }
    const size_t start = (m_preroll_write + kPrerollSamples - (size_t)n) % kPrerollSamples;
    const size_t first = kPrerollSamples - start;
    if ((size_t)n <= first) {
        memcpy(dst, &m_preroll[start], (size_t)n * sizeof(int16_t));
    } else {
        memcpy(dst, &m_preroll[start], first * sizeof(int16_t));
        memcpy(dst + first, m_preroll, ((size_t)n - first) * sizeof(int16_t));
    }

    unlock_state();
    return n;
}

void Wake::preroll_clear()
{
    if (!lock_state()) {
        return;
    }
    m_preroll_len = 0;
    m_preroll_write = 0;
    unlock_state();
}

void Wake::process_frame()
{
    memset(m_fft_buf, 0, sizeof(m_fft_buf));

    for (int i = 0; i < kFrameLen; i++) {
        m_fft_buf[i] = m_pcm_window[i] * m_hann_window[i];
    }

    dl_rfft_f32_run(m_fft, m_fft_buf);

    /* Вычисление спектра мощности */
    m_power_spec[0] = (m_fft_buf[0] * m_fft_buf[0]) / (float)kFrameLen;
    m_power_spec[kFftPoints / 2] = (m_fft_buf[1] * m_fft_buf[1]) / (float)kFrameLen;
    for (int k = 1; k < kFftPoints / 2; k++) {
        float re = m_fft_buf[2 * k];
        float im = m_fft_buf[2 * k + 1];
        m_power_spec[k] = (re * re + im * im) / (float)kFrameLen;
    }

    /* Mel-фильтры */
    float log_mel[kMelBins];
    for (int m = 0; m < kMelBins; m++) {
        int left = kMelBinEdges[m];
        int center = kMelBinEdges[m + 1];
        int right = kMelBinEdges[m + 2];
        float energy = 0.0f;

        for (int k = left; k < center; k++) {
            float w = (float)(k - left) / (float)(center - left);
            energy += m_power_spec[k] * w;
        }
        for (int k = center; k < right; k++) {
            float w = (float)(right - k) / (float)(right - center);
            energy += m_power_spec[k] * w;
        }
        log_mel[m] = logf(energy + 1e-6f);
    }

    /* Сдвиг матрицы спектрограммы на 1 фрейм влево */
    memmove(m_spectrogram, m_spectrogram + kMelBins,
            (kModelFrames - 1) * kMelBins * sizeof(float));
    memcpy(m_spectrogram + (kModelFrames - 1) * kMelBins,
           log_mel, kMelBins * sizeof(float));
}

bool Wake::feed(const int16_t *pcm, int samples)
{
    if (!m_ready.load(std::memory_order_acquire) ||
        !m_config.wake_enabled() || !pcm || samples <= 0) {
        return false;
    }

    if (esp_timer_get_time() < m_mute_until_us.load(std::memory_order_acquire)) {
        return false;
    }

    /* Буфер предзаписи наполняем всегда, когда детектор не заглушён. */
    {
        size_t n = (size_t)samples;
        const int16_t *src = pcm;
        while (n > 0) {
            const size_t space = kPrerollSamples - m_preroll_write;
            const size_t take = (n < space) ? n : space;
            memcpy(&m_preroll[m_preroll_write], src, take * sizeof(int16_t));
            m_preroll_write = (m_preroll_write + take) % kPrerollSamples;
            if (m_preroll_len < kPrerollSamples) {
                m_preroll_len += take;
                if (m_preroll_len > kPrerollSamples) {
                    m_preroll_len = kPrerollSamples;
                }
            }
            src += take;
            n -= take;
        }
    }

    if (!lock_state()) {
        return false;
    }

    if (!m_initialized || !m_fft || !m_spectrogram || !m_norm_spec || !m_infer_arena ||
        esp_timer_get_time() < m_mute_until_us.load(std::memory_order_acquire)) {
        unlock_state();
        return false;
    }

    bool detected = false;
    const float threshold = (float)m_config.wake_threshold() / 100.0f;

    for (int i = 0; i < samples; i++) {
        int16_t val = pcm[i];
        int16_t abs_val = (val < 0) ? (int16_t)-val : val;
        if (abs_val > m_step_peak) {
            m_step_peak = abs_val;
        }

        m_step_buf[m_step_buf_len++] = (float)val / 32768.0f;

        if (m_step_buf_len >= kFrameStep) {
            m_step_buf_len = 0;

            /* Сохраняем пиковую амплитуду за шаг 160 сэмплов */
            memmove(m_frame_peak, m_frame_peak + 1, (kModelFrames - 1) * sizeof(int16_t));
            m_frame_peak[kModelFrames - 1] = m_step_peak;
            m_step_peak = 0;

            /* Блочный сдвиг окна PCM на FRAME_STEP сэмплов */
            memmove(m_pcm_window, m_pcm_window + kFrameStep, (kFrameLen - kFrameStep) * sizeof(float));
            memcpy(m_pcm_window + (kFrameLen - kFrameStep), m_step_buf, kFrameStep * sizeof(float));

            process_frame();
            if (m_frames_collected < kModelFrames) {
                m_frames_collected++;
            }
            m_step_counter++;

            /* Инференс каждые 10 фреймов (каждые 100 мс) для более отзывчивой реакции */
            if (m_step_counter >= 10) {
                m_step_counter = 0;

                /* Не запускаем инференс первые 30 фреймов (300 мс) после сброса */
                if (m_frames_collected < 30) {
                    m_consecutive_hits = 0;
                    continue;
                }

                /* Энергетический фильтр: отсекает аппаратный ноль и глубокую тишину */
                int16_t window_max = 0;
                for (int f = 0; f < kModelFrames; f++) {
                    if (m_frame_peak[f] > window_max) {
                        window_max = m_frame_peak[f];
                    }
                }

                if (window_max < 500) {
                    m_consecutive_hits = 0;
                    continue;
                }

                /* Стандартизация спектрограммы (mean 0, std 1) */
                float sum = 0.0f;
                float sq_sum = 0.0f;
                for (int c = 0; c < kTotalCells; c++) {
                    float v = m_spectrogram[c];
                    sum += v;
                    sq_sum += v * v;
                }
                float mean = sum / (float)kTotalCells;
                float var = (sq_sum / (float)kTotalCells) - (mean * mean);
                float std = sqrtf((var > 1e-6f) ? var : 1e-6f);

                for (int c = 0; c < kTotalCells; c++) {
                    m_norm_spec[c] = (m_spectrogram[c] - mean) / std;
                }

                float prob = wake_model_ey_gin_infer(m_norm_spec, m_infer_arena);

                /* Диагностика: видно почти-срабатывания, по ним подбирается порог. */
                if (prob >= 0.5f) {
                    ESP_LOGD(TAG, "wake prob=%.3f (порог %.2f)", (double)prob, (double)threshold);
                }

                bool hit = false;
                if (prob >= threshold) {
                    m_consecutive_hits++;
                    if (m_consecutive_hits >= 2) {
                        hit = true;
                    }
                } else {
                    m_consecutive_hits = 0;
                }

                if (hit) {
                    m_consecutive_hits = 0;
                    reset_locked();
                    if (esp_timer_get_time() >= m_mute_until_us.load(std::memory_order_acquire)) {
                        ESP_LOGI(TAG, "распознано «эй гин»! prob=%.3f (порог NVS=%.2f)",
                                 (double)prob, (double)threshold);
                        detected = true;
                    }
                }
            }
        }
    }

    unlock_state();
    return detected;
}

Wake &wake()
{
    static Wake s_instance(config(), http());
    return s_instance;
}

}  // namespace kouki

// Тонкий C-фасад: тела — по одной строке, поведение и esp_err_t 1:1 с прежним
// app_wake.c. Публичный app_wake.h не менялся.
extern "C" {

esp_err_t kouki_wake_init(void)
{
    return kouki::wake().init();
}

void kouki_wake_deinit(void)
{
    kouki::wake().deinit();
}

bool kouki_wake_is_ready(void)
{
    return kouki::wake().is_ready();
}

const char *kouki_wake_model(void)
{
    return kouki::wake().model();
}

void kouki_wake_mute(int duration_ms)
{
    kouki::wake().mute(duration_ms);
}

void kouki_wake_reset(void)
{
    kouki::wake().reset();
}

int kouki_wake_get_preroll(int16_t *dst, int max_samples)
{
    return kouki::wake().get_preroll(dst, max_samples);
}

void kouki_wake_preroll_clear(void)
{
    kouki::wake().preroll_clear();
}

bool kouki_wake_feed(const int16_t *pcm, int samples)
{
    return kouki::wake().feed(pcm, samples);
}

}  // extern "C"
