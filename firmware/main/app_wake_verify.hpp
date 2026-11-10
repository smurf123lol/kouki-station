#ifndef APP_WAKE_VERIFY_HPP
#define APP_WAKE_VERIFY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: подписи kouki_wake_verify_* и
// kouki_wake_verify_state_t остаются общими для C- и C++-модулей.
#include "app_wake_verify.h"

// FreeRTOS.h обязан идти раньше task.h — то же требование, что в IDF.
// На host-стенде эти заголовки подменяются моками из firmware/test/support/.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace kouki {

class Config;
class Http;

/**
 * ASR-подтверждение кодового слова (T121 П9).
 *
 * Локальный детектор не отличает «эй гин» от громкой посторонней речи, поэтому
 * после хитра станция отправляет первый кусок записи на POST /api/transcribe и
 * сверяет распознанный текст. Класс владеет состоянием проверки (m_state),
 * дескриптором задачи (m_task), последним распознанным текстом (m_transcript) и
 * незавершённым заданием (m_job) — прежние `static`-переменные
 * app_wake_verify.c:23-25.
 *
 * Зависимости приходят в конструктор: Config — адрес/токен сервера, Http —
 * создание клиента с pinned CA и диагностика ошибок соединения.
 *
 * Экземпляр один (см. kouki::wake_verify()), создаётся статически.
 */
class WakeVerify {
public:
    WakeVerify(Config &config, Http &http);
    WakeVerify(const WakeVerify &) = delete;
    WakeVerify &operator=(const WakeVerify &) = delete;

    /**
     * Запускает проверку куска PCM (16 кГц, моно, int16) на сервере: копирует
     * данные в WAV-буфер и создаёт задачу. Не блокирует вызывающего.
     *
     * @return ESP_OK — задача создана; ESP_ERR_INVALID_ARG — пустой PCM;
     *         ESP_ERR_INVALID_STATE — предыдущая проверка ещё идёт;
     *         ESP_ERR_NO_MEM — не хватило памяти.
     */
    esp_err_t start(const int16_t *pcm, int samples);

    /** Текущее состояние проверки (читается из FSM без блокировки). */
    kouki_wake_verify_state_t state() const;

    /** Сбрасывает состояние в IDLE (новая запись). Задачу не убивает. */
    void reset();

    /** Последний распознанный текст (пустая строка, если не было). */
    const char *last_transcript() const;

    /**
     * Проверяет, похож ли текст ASR на «эй гин»: характерное слово семейства
     * «гин/дин/жин» либо короткое обращение в начале. Выделено для тестов.
     */
    static bool matches(const char *transcript);

private:
    /** Задание на проверку: WAV целиком в psram/куче. */
    struct Job {
        uint8_t *wav;
        size_t wav_len;
    };

    /** Точка входа задачи FreeRTOS: arg — указатель на этот экземпляр. */
    static void task_entry(void *arg);

    /** Тело задачи: POST /api/transcribe, разбор ответа, фиксация состояния. */
    void run();

    Config &m_config;
    Http &m_http;

    std::atomic<int> m_state{KOUKI_WAKE_VERIFY_IDLE};
    TaskHandle_t m_task = nullptr;
    Job *m_job = nullptr;
    char m_transcript[160]{};
};

/** Единственный экземпляр ASR-подтверждения. */
WakeVerify &wake_verify();

}  // namespace kouki

#endif  // APP_WAKE_VERIFY_HPP
