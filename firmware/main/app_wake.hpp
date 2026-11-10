#ifndef APP_WAKE_HPP
#define APP_WAKE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: подписи kouki_wake_* и типы из app_wake.h
// остаются общими для C- и C++-модулей.
#include "app_wake.h"

// FreeRTOS.h обязан идти раньше semphr.h — то же требование, что в
// IDF-заголовках: иначе semphr.h не находит portMAX_DELAY/TickType_t.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "dl_rfft.h"

namespace kouki {

class Config;
class Http;

/**
 * Детектор кодового слова «эй гин» (T121 П9).
 *
 * Владеет всем состоянием модуля как членами: leaf-мьютексом состояния,
 * спинлоком ленивого создания мьютекса, признаком готовности, отметкой mute,
 * FFT-инстансом, окнами/буферами кадра, спектрограммой, ареной активаций и
 * кольцом предзаписи (~1 с PCM). Прежние файловые `static`-переменные
 * (app_wake.c:38-65) переехали в класс; function-local static-буферы
 * process_frame() — тоже: их больше не нужно защищать отдельным признаком
 * инициализации.
 *
 * Зависимости приходят в конструктор: разрешение и порог детектора — из
 * Config (wake_enabled()/wake_threshold()), Http принимается по спеке §2 как
 * зависимость модуля (сам детектор в сеть не ходит — ASR-подтверждение живёт
 * в отдельном классе WakeVerify, app_wake_verify.cpp).
 *
 * ИНВАРИАНТ (AGENTS.md п.1): у I2S один владелец. Wake не читает микрофон и
 * не зовёт esp_get_feed_data/esp_codec_dev_read: PCM приходит ему готовым
 * чанком из record_task через feed(). Новых читателей микрофона класс не
 * заводит.
 *
 * Экземпляр один (см. kouki::wake()), создаётся статически при первом
 * обращении; деструктора нет — модуль живёт до перезагрузки.
 */
class Wake {
public:
    /** Размер FFT (512 точек — 32 мс при 16 кГц). */
    static constexpr int kFftPoints = 512;
    /** Длина кадра анализа: 30 мс при 16 кГц. */
    static constexpr int kFrameLen = 480;
    /** Шаг окна: 10 мс при 16 кГц. */
    static constexpr int kFrameStep = 160;
    /** Число mel-фильтров. */
    static constexpr int kMelBins = 40;
    /**
     * Число кадров модели (1 с при шаге 10 мс). Значение обязано совпадать с
     * WAKE_MODEL_FRAMES из wake_model_ey_gin.h — это проверяет static_assert
     * в app_wake.cpp. Саму 82-килобайтную модель в заголовок не тянем, чтобы
     * .hpp оставался лёгким для потребителей.
     */
    static constexpr int kModelFrames = 148;
    /** Размер кольца предзаписи: 16000 сэмплов = 1 с при 16 кГц. */
    static constexpr size_t kPrerollSamples = 16000;

    Wake(Config &config, Http &http);
    Wake(const Wake &) = delete;
    Wake &operator=(const Wake &) = delete;

    /**
     * Поднимает детектор: FFT-инстанс, окно Ханна, буферы спектрограммы,
     * нормализации и арены активаций, затем глушит детектор на 2.5 с
     * (щелчки включения микрофона). Повторный вызов — ESP_OK без работы.
     *
     * @return ESP_OK — готов; ESP_ERR_NOT_SUPPORTED — wake_en=0 в NVS;
     *         ESP_ERR_NO_MEM — не хватило памяти под буферы.
     */
    esp_err_t init();

    /** Освобождает буферы и сбрасывает состояние. Повторный init разрешён. */
    void deinit();

    /** true, если детектор готов и в конфигурации включён wake_en. */
    bool is_ready();

    /** Имя модели, которую реально слушаем (для лога и дашборда). */
    const char *model() const;

    /**
     * Подавляет детектор на duration_ms миллисекунд (после тонов/речи через
     * динамик — чтобы не было акустической обратной связи) и сбрасывает
     * накопленные буферы.
     */
    void mute(int duration_ms);

    /** Сбрасывает спектрограмму, PCM-окно и счётчики. */
    void reset();

    /** Копирует последние ~1 с микрофона (хронологический порядок). */
    int get_preroll(int16_t *dst, int max_samples);

    /** Очищает кольцо предзаписи (например, после ложного хита). */
    void preroll_clear();

    /**
     * Проверяет чанк PCM 16 кГц/моно: копит шаги по 160 сэмплов, каждые
     * 100 мс прогоняет инференс и требует двух подряд хитов выше порога.
     *
     * @return true, если в этом чанке услышали кодовое слово.
     */
    bool feed(const int16_t *pcm, int samples);

private:
    /** Берёт leaf-мьютекс состояния (создавая его при необходимости). */
    bool lock_state();

    /** Отпускает leaf-мьютекс состояния. */
    void unlock_state();

    /** Сбрасывает накопленное. Вызывать под взятым мьютексом. */
    void reset_locked();

    /** Считает mel-спектр текущего окна и сдвигает матрицу на кадр. */
    void process_frame();

    Config &m_config;
    Http &m_http;

    /*
     * Leaf-мьютекс состояния (Level 2b): никогда не берёт m_lock и
     * audio_out. Защищает буферы инференса/счётчики от гонок между
     * record_task (Core 1) и reset()/mute()/deinit() из других задач
     * (Core 0).
     */
    SemaphoreHandle_t m_mutex = nullptr;
    // Защищает только ленивое создание m_mutex.
    portMUX_TYPE m_spinlock = portMUX_INITIALIZER_UNLOCKED;
    dl_fft_f32_t *m_fft = nullptr;

    // Признак «готов принимать звук» читают другие задачи без мьютекса.
    std::atomic<bool> m_ready{false};
    std::atomic<int64_t> m_mute_until_us{0};

    bool m_initialized = false;
    /* Выравнивание 16 байт, как у файловых static-массивов прежнего модуля:
     * dl_rfft_f32_run() на ESP32-S3 считает SIMD-ядром и читает вход
     * 128-битными блоками — буфер-член класса без alignas выровнен только по
     * 4 байта, и спектр получается мусорным (на host-стенде не видно: там
     * наивный DFT). */
    alignas(16) float m_hann_window[kFrameLen]{};
    alignas(16) float m_pcm_window[kFrameLen]{};
    alignas(16) float m_step_buf[kFrameStep]{};
    // Рабочие буферы process_frame(): раньше это были function-local static
    // внутри process_frame() (app_wake.c:316, :326) — теперь члены класса,
    // чтобы состояние не жило вне объекта и не требовало отдельного признака
    // инициализации.
    alignas(16) float m_fft_buf[kFftPoints]{};
    alignas(16) float m_power_spec[kFftPoints / 2 + 1]{};
    int m_step_buf_len = 0;
    float *m_spectrogram = nullptr;
    float *m_norm_spec = nullptr;
    float *m_infer_arena = nullptr;
    int m_step_counter = 0;
    int m_frames_collected = 0;
    int16_t m_frame_peak[kModelFrames]{};
    int16_t m_step_peak = 0;
    int m_consecutive_hits = 0;

    /*
     * Кольцо последних ~1 с PCM. Нужно для двух вещей: подмешать начало
     * записи, чтобы не потерять «эй гин» и первый слог команды, и отправить
     * этот же кусок на ASR-подтверждение.
     */
    int16_t m_preroll[kPrerollSamples]{};
    size_t m_preroll_len = 0;
    size_t m_preroll_write = 0;
};

/*
 * Проверка на этапе компиляции: dl_rfft_f32_run() на ESP32-S3 читает вход
 * 128-битными блоками, поэтому рабочие буферы обязаны быть выровнены по 16
 * байт. Класс без alignas-полей теряет это выравнивание (был реальный провал
 * сценария wake: спектр считался мусором, вероятность хита 0.47 вместо 0.99),
 * и на host-стенде это не видно — там вместо esp-dl работает наивный DFT.
 */
static_assert(alignof(Wake) >= 16, "буферы Wake должны быть выровнены по 16 байт: dl_rfft_f32_run на S3 — SIMD");

/** Единственный экземпляр детектора кодового слова. */
Wake &wake();

}  // namespace kouki

#endif  // APP_WAKE_HPP
