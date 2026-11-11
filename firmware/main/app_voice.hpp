#ifndef APP_VOICE_HPP
#define APP_VOICE_HPP

#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: подписи kouki_voice_* и типы результата
// остаются общими для C- и C++-модулей.
#include "app_voice.h"

#include "fsm/voice_state_manager.hpp"
#include "fsm/states/done_state.hpp"
#include "fsm/states/error_state.hpp"
#include "fsm/states/followup_state.hpp"
#include "fsm/states/idle_state.hpp"
#include "fsm/states/recording_state.hpp"
#include "fsm/states/sending_state.hpp"

namespace kouki {

class Config;
class Vad;
class Wake;

/**
 * Запись голоса с микрофона и мост к C++ FSM (T121 П12).
 *
 * Владеет всем состоянием модуля как членами: самим автоматом
 * VoiceStateManager, шестью объектами состояний, буфером записи в PSRAM и
 * рабочими буферами задачи записи. Прежние файловые `static`
 * (app_voice.cpp:32-41) переехали в класс; function-local static-буферы
 * read_mic() и record_task() — тоже.
 *
 * ИНВАРИАНТ (AGENTS.md п.1): у I2S один владелец. Микрофон читает только
 * задача записи (record_task_entry → read_mic → esp_get_feed_data); детектор
 * кодового слова получает готовый PCM из того же потока через FSM
 * (IdleState::on_step → Wake::feed), а не читает I2S сам. Других вызовов
 * esp_get_feed_data в прошивке быть не должно — кроме диагностики
 * debug_levels(), которая работает только по явной команде оператора.
 *
 * Зависимости приходят в конструктор (спека §2): Config — адрес сервера для
 * стартового лога, Vad — детектор речевой активности, Wake — детектор
 * кодового слова; оба инициализируются в init() вместо C-фасадов П6/П9.
 *
 * Экземпляр один (см. kouki::voice()), создаётся статически при первом
 * обращении; деструктора нет — модуль живёт до перезагрузки.
 */
class Voice {
public:
    Voice(Config &config, Vad &vad, Wake &wake);
    Voice(const Voice &) = delete;
    Voice &operator=(const Voice &) = delete;

    /**
     * Готовит буфер WAV под запись (в PSRAM на плате), регистрирует состояния
     * в автомате, инициализирует VAD и поднимает задачу записи.
     *
     * @param cb  колбэк смены состояния (может быть nullptr);
     * @param ctx пользовательский контекст колбэка.
     * @return ESP_OK, ESP_ERR_NO_MEM (нет PSRAM/задачи) или ESP_ERR_* модулей.
     */
    esp_err_t init(kouki_voice_event_cb_t cb, void *ctx);

    /** Выбирает сессию до начала записи; ESP_ERR_INVALID_STATE — идёт запись. */
    esp_err_t set_session(kouki_session_t session);
    kouki_session_t session() const;

    /** Начать запись (кнопка); ESP_ERR_INVALID_STATE — нет сети или идёт отправка. */
    esp_err_t start_recording();

    /** Попросить закончить запись: остановку и отправку делает задача записи. */
    esp_err_t stop_and_send();

    /** Диагностика: печатает уровень каждого канала входа. */
    void debug_levels();

    kouki_voice_state_t state() const;
    esp_err_t dismiss_error();
    bool is_recording() const;
    bool get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result) const;
    void get_metrics(kouki_voice_metrics_t *metrics) const;
    void print_state() const;

#if defined(CONFIG_KOUKI_TEST_CONSOLE_CMDS)
    /** Отладочный хук: переводит FSM в DONE с макетом ответа. */
    esp_err_t test_mock_done(const kouki_voice_result_t &result);
#endif

private:
    /** Тело задачи записи: читает микрофон и шагает автоматом. */
    void record_loop();
    static void record_task_entry(void *arg);

    /**
     * Читает один кусок с микрофона и отдаёт выбранный канал в dst.
     * Единственное (кроме debug_levels) место вызова esp_get_feed_data.
     */
    esp_err_t read_mic(int16_t *dst, int frames);

    kouki::voice::VoiceStateManager m_manager;
    kouki::voice::IdleState m_idle_state;
    kouki::voice::RecordingState m_recording_state;
    kouki::voice::SendingState m_sending_state;
    kouki::voice::DoneState m_done_state;
    kouki::voice::FollowupState m_followup_state;
    kouki::voice::ErrorState m_error_state;

    uint8_t *m_buf = nullptr;
    size_t m_capacity_bytes = 0;

    // Рабочие буферы: 16-байтное выравнивание — требование DMA/накопителей
    // (правило пакета T121 для буферов, читаемых блоками).
    alignas(16) int16_t m_mic_raw[kouki::voice::RECORD_CHUNK * 4]{};
    alignas(16) int16_t m_mono[kouki::voice::RECORD_CHUNK]{};

    Config &m_config;
    Vad &m_vad;
    Wake &m_wake;
};

static_assert(alignof(Voice) >= 16, "буферы записи требуют выравнивания 16 байт");

/**
 * Единственный экземпляр голосового ядра.
 *
 * Имя не `voice()`: в namespace kouki уже есть namespace kouki::voice (FSM),
 * и фабрика с тем же именем конфликтует с ним по объявлению.
 */
Voice &voice_engine();

}  // namespace kouki

#endif  // APP_VOICE_HPP
