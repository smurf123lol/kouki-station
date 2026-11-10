// Голосовое ядро станции (T121 П12): класс kouki::Voice вместо C-модуля
// со static-состоянием.
//
// Состояние модуля (прежние static app_voice.cpp:32-41) живёт в экземпляре
// kouki::voice(): автомат VoiceStateManager, шесть объектов состояний, буфер
// записи в PSRAM и рабочие буферы задачи записи. C-API kouki_voice_* в конце
// файла — тонкий фасад: тело каждого метода — один вызов класса.
//
// ИНВАРИАНТ I2S (AGENTS.md п.1): микрофон читает только задача записи
// (record_task_entry -> read_mic -> esp_get_feed_data). Детектор кодового
// слова получает готовый PCM из того же потока через FSM
// (IdleState::on_step -> kouki_wake_feed), а не открывает I2S сам. Второй
// вызов esp_get_feed_data в файле есть только в debug_levels() — это
// диагностика по явной команде оператора, её не расширяем.
#include "app_voice.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>

#include "app_config.hpp"
#include "app_vad.hpp"
#include "app_wake.hpp"

#ifdef ESP_PLATFORM
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Типы и константы канала записи: те же, что были у C-модуля.
#include "app_tone.h"
#include "app_wifi.h"
#include "esp_board_init.h"
#endif

namespace kouki {

namespace {

const char *const kTag = "kouki.voice";

/** Каналов в потоке I2S: 4 (raw) или 3 (обработанный). */
constexpr int kRawChannels = 4;

}  // namespace

Voice::Voice(Config &config, Vad &vad, Wake &wake)
    : m_config(config), m_vad(vad), m_wake(wake)
{
    // Зависимости принимаются по спеке §2: Config даёт адрес сервера для
    // стартового лога, Vad и Wake инициализируются в init(). Прямых вызовов
    // в конструкторе нет — экземпляр создаётся статически до готовности
    // NVS/радио.
}

esp_err_t Voice::init(kouki_voice_event_cb_t cb, void *ctx)
{
#ifdef ESP_PLATFORM
    m_capacity_bytes = static_cast<size_t>(CONFIG_KOUKI_MAX_RECORD_S) *
                       kouki::voice::SAMPLE_RATE * kouki::voice::BYTES_PER_SAMPLE;
    m_buf = static_cast<uint8_t *>(heap_caps_malloc(kouki::voice::WAV_HEADER_SIZE + m_capacity_bytes,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (m_buf == nullptr) {
        ESP_LOGE(kTag, "не хватило PSRAM под %u байт записи",
                 static_cast<unsigned>(kouki::voice::WAV_HEADER_SIZE + m_capacity_bytes));
        return ESP_ERR_NO_MEM;
    }
#else
    m_capacity_bytes = 15 * kouki::voice::SAMPLE_RATE * kouki::voice::BYTES_PER_SAMPLE;
    m_buf = static_cast<uint8_t *>(std::malloc(kouki::voice::WAV_HEADER_SIZE + m_capacity_bytes));
#endif

    // Регистрируем состояния в менеджере
    m_manager.register_state(&m_idle_state);
    m_manager.register_state(&m_recording_state);
    m_manager.register_state(&m_sending_state);
    m_manager.register_state(&m_done_state);
    m_manager.register_state(&m_followup_state);
    m_manager.register_state(&m_error_state);

    // Настраиваем буфер в контексте
    m_manager.context().buffer = m_buf;
    m_manager.context().capacity_bytes = m_capacity_bytes;
    m_manager.context().sample_bytes = 0;

    // Инициализируем FSM
    m_manager.init(cb, ctx);
    m_vad.init();

#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(record_task_entry, "kouki.record", 8192, this, 5, nullptr, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (m_wake.init() != ESP_OK) {
        ESP_LOGI(kTag, "wake-word деактивирован или недоступен, работаем по кнопке");
    }

    ESP_LOGI(kTag, "C++ FSM голосовое ядро запущено: макс %d с, сервер %s",
             CONFIG_KOUKI_MAX_RECORD_S, m_config.srv_url());
#endif

    return ESP_OK;
}

esp_err_t Voice::set_session(kouki_session_t session)
{
    return m_manager.try_set_session(session) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

kouki_session_t Voice::session() const
{
    return m_manager.get_session();
}

esp_err_t Voice::start_recording()
{
    if (m_manager.current_id() == kouki::voice::StateId::Sending) {
        return ESP_ERR_INVALID_STATE;
    }
#ifdef ESP_PLATFORM
    if (!kouki_wifi_is_connected()) {
        kouki_tone_play(KOUKI_TONE_ERROR);
        ESP_LOGW(kTag, "запись не началась: нет подключения к Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    return m_manager.start_recording_from_button() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t Voice::stop_and_send()
{
    m_manager.stop_and_send_from_button();
    return ESP_OK;
}

void Voice::debug_levels()
{
#ifdef ESP_PLATFORM
    // Рабочий буфер диагностики — как в master: function-local static, а не
    // член класса. При выключенном CONFIG_KOUKI_DEBUG_MIC функция не
    // вызывается, и линкер с --gc-sections выбрасывает её вместе с буфером;
    // член класса остался бы в .bss всегда и съел 8 КБ внутренней RAM.
    static int16_t debug_raw[kouki::voice::RECORD_CHUNK * kRawChannels];
    const int bytes = static_cast<int>(kouki::voice::RECORD_CHUNK) * kRawChannels *
                      static_cast<int>(kouki::voice::BYTES_PER_SAMPLE);

    for (int mode = 0; mode < 2; mode++) {
        const bool get_raw = (mode == 0);
        const int channels = get_raw ? kRawChannels : 3;
        uint64_t sums[4] = {0};
        int chunks = 0;

        for (int c = 0; c < 12; c++) {
            // Диагностика: второй (и последний) читатель микрофона в файле.
            // Работает только по явной команде оператора, не в потоке записи.
            if (esp_get_feed_data(get_raw, debug_raw, bytes) != ESP_OK) {
                ESP_LOGE(kTag, "диагностика: чтение не удалось (raw=%d)", get_raw);
                continue;
            }
            for (int i = 0; i < static_cast<int>(kouki::voice::RECORD_CHUNK); i++) {
                for (int ch = 0; ch < channels; ch++) {
                    sums[ch] += static_cast<uint64_t>(std::abs(debug_raw[i * channels + ch]));
                }
            }
            chunks++;
        }

        if (chunks == 0) continue;

        const uint64_t divisor = static_cast<uint64_t>(chunks) * kouki::voice::RECORD_CHUNK;
        ESP_LOGI(kTag, "уровни каналов (raw=%d, каналов %d): %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
                 get_raw, channels,
                 sums[0] / divisor,
                 channels > 1 ? sums[1] / divisor : 0,
                 channels > 2 ? sums[2] / divisor : 0,
                 channels > 3 ? sums[3] / divisor : 0);
    }
#endif
}

kouki_voice_state_t Voice::state() const
{
    return kouki::voice::to_c_state(m_manager.current_id());
}

esp_err_t Voice::dismiss_error()
{
    return m_manager.dismiss_error() ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool Voice::is_recording() const
{
    return m_manager.current_id() == kouki::voice::StateId::Recording;
}

bool Voice::get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result) const
{
    return m_manager.get_snapshot(state, result);
}

void Voice::get_metrics(kouki_voice_metrics_t *metrics) const
{
    m_manager.get_metrics(metrics);
}

void Voice::print_state() const
{
    const kouki::voice::FsmDiagnostics d = m_manager.get_diagnostics();
    kouki_voice_metrics_t m{};
    m_manager.get_metrics(&m);
    const double seconds = static_cast<double>(d.buffer_used / kouki::voice::BYTES_PER_SAMPLE) /
                           static_cast<double>(kouki::voice::SAMPLE_RATE);

    std::printf("\n--- FSM Состояние ---\n");
    std::printf("Текущее:       %s\n", d.current_name);
    std::printf("В состоянии:   %u мс\n", static_cast<unsigned>(d.time_in_state_ms));
    std::printf("Предыдущее:    %s\n", d.previous_name);
    std::printf("Причина входа: %s\n", d.last_reason ? d.last_reason : "none");
    std::printf("Всего переходов: %u\n", static_cast<unsigned>(d.transition_count));
    std::printf("Прерываний:    %u\n", static_cast<unsigned>(d.interrupt_count));
    std::printf("Буфер аудио:   %u / %u байт (%.1f сек)\n",
                static_cast<unsigned>(d.buffer_used), static_cast<unsigned>(d.buffer_capacity), seconds);
    std::printf("VAD метрики:   речь=%s, тишина=%u/%u мс, длит=%u мс\n",
                m.speech_detected ? "да" : "нет",
                static_cast<unsigned>(m.silence_ms),
                static_cast<unsigned>(m.silence_limit_ms),
                static_cast<unsigned>(m.record_duration_ms));
    std::printf("---------------------\n\n");
}

#if defined(CONFIG_KOUKI_TEST_CONSOLE_CMDS)
esp_err_t Voice::test_mock_done(const kouki_voice_result_t &result)
{
    return m_manager.force_done_for_test(result) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
#endif

#ifdef ESP_PLATFORM
esp_err_t Voice::read_mic(int16_t *dst, int frames)
{
    const int channels = esp_get_feed_channel() > 0 ? esp_get_feed_channel() : kRawChannels;
    const int mic = CONFIG_KOUKI_MIC_CHANNEL < channels ? CONFIG_KOUKI_MIC_CHANNEL : 0;

    if (mic >= kRawChannels || frames > static_cast<int>(kouki::voice::RECORD_CHUNK)) {
        return ESP_ERR_INVALID_STATE;
    }

    const int bytes = frames * channels * static_cast<int>(kouki::voice::BYTES_PER_SAMPLE);
    // Единственное чтение микрофона в потоке записи.
    ESP_RETURN_ON_ERROR(esp_get_feed_data(true, m_mic_raw, bytes), kTag, "чтение микрофона не удалось");

    for (int i = 0; i < frames; i++) {
        int32_t value = static_cast<int32_t>(m_mic_raw[i * channels + mic]) * CONFIG_KOUKI_MIC_GAIN;
        if (value > INT16_MAX) {
            value = INT16_MAX;
        } else if (value < INT16_MIN) {
            value = INT16_MIN;
        }
        dst[i] = static_cast<int16_t>(value);
    }

    return ESP_OK;
}

void Voice::record_loop()
{
    while (true) {
        if (read_mic(m_mono, static_cast<int>(kouki::voice::RECORD_CHUNK)) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            m_manager.step(nullptr, 0);
            continue;
        }

        m_manager.step(m_mono, kouki::voice::RECORD_CHUNK);
    }
}

void Voice::record_task_entry(void *arg)
{
    static_cast<Voice *>(arg)->record_loop();
}
#endif

Voice &voice_engine()
{
    static Voice s_instance(config(), vad(), wake());
    return s_instance;
}

}  // namespace kouki

extern "C" {

esp_err_t kouki_voice_init(kouki_voice_event_cb_t cb, void *ctx)
{
    return kouki::voice_engine().init(cb, ctx);
}

esp_err_t kouki_voice_set_session(kouki_session_t session)
{
    return kouki::voice_engine().set_session(session);
}

kouki_session_t kouki_voice_get_session(void)
{
    return kouki::voice_engine().session();
}

esp_err_t kouki_voice_start_recording(void)
{
    return kouki::voice_engine().start_recording();
}

esp_err_t kouki_voice_stop_and_send(void)
{
    return kouki::voice_engine().stop_and_send();
}

void kouki_voice_debug_levels(void)
{
    kouki::voice_engine().debug_levels();
}

kouki_voice_state_t kouki_voice_get_state(void)
{
    return kouki::voice_engine().state();
}

esp_err_t kouki_voice_dismiss_error(void)
{
    return kouki::voice_engine().dismiss_error();
}

bool kouki_voice_is_recording(void)
{
    return kouki::voice_engine().is_recording();
}

bool kouki_voice_get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result)
{
    return kouki::voice_engine().get_snapshot(state, result);
}

void kouki_voice_get_metrics(kouki_voice_metrics_t *metrics)
{
    kouki::voice_engine().get_metrics(metrics);
}

void kouki_voice_print_state(void)
{
    kouki::voice_engine().print_state();
}

#if defined(CONFIG_KOUKI_TEST_CONSOLE_CMDS)
esp_err_t kouki_voice_test_mock_done(const kouki_voice_result_t *result)
{
    if (result == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    return kouki::voice_engine().test_mock_done(*result);
}
#endif

}  // extern "C"
