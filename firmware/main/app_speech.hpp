#ifndef APP_SPEECH_HPP
#define APP_SPEECH_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: подписи kouki_speech_* и
// kouki_speech_wav_t остаются общими для C- и C++-модулей.
#include "app_speech.h"

// Тип события нужен в подписи C-колбэка HTTP-клиента.
#include "esp_http_client.h"

namespace kouki {

class AudioOut;
class Config;
class Tone;

/**
 * Озвучивание ответа станции (T121 П10).
 *
 * Сервер синтезирует речь сам и отдаёт WAV 16 кГц моно; модуль забирает файл
 * по чанкам и разворачивает моно в стереопару L=R, потому что воспроизведение
 * открыто в стерео (main.c: esp_board_init(16000, 2, 16)), а кодек трактует
 * поток как чередующиеся каналы.
 *
 * Состояние — члены класса, прежние `static` app_speech.c:88-92:
 * счётчики поколений (m_latest_gen, m_aborted_up_to_gen), признак активного и
 * звучащего поколения (m_active_gen, m_speaking_gen) и число живых воркеров
 * (m_active_workers). Поколение — единственный механизм отмены: abort()
 * поднимает m_aborted_up_to_gen, а воркер сверяет его на каждом этапе HTTP и
 * между чанками звука.
 *
 * Зависимости приходят в конструктор: Config — адрес/токен сервера и режим
 * тишины, AudioOut — монопольный доступ к кодеку (Level 2 той же иерархии
 * блокировок), Tone — сигналы станции (нужен вызывающему для общей фабрики,
 * сам Speech сигналов не играет).
 *
 * Экземпляр один (см. kouki::speech()), создаётся статически.
 */
class Speech {
public:
    Speech(Config &config, AudioOut &audio, Tone &tone);
    Speech(const Speech &) = delete;
    Speech &operator=(const Speech &) = delete;

    /**
     * Синтезирует и проигрывает текст (блокирует до конца звучания).
     * Равнозначно play_gen(text, 0).
     */
    esp_err_t play(const char *text);

    /**
     * Резервирует поколение речи синхронно до создания фоновой задачи, чтобы
     * abort() отменял запуск ещё до старта play_gen().
     */
    uint32_t reserve();

    /** Вариант play() с заранее зарезервированным поколением (0 — выделить внутри). */
    esp_err_t play_gen(const char *text, uint32_t gen);

    /** Идёт HTTP-синтез, чанки или хвост DMA — откладывает запись и OTA. */
    bool busy() const;

    /** Активен ли воркер речи (предотвращает запуск OTA во время очистки фразы). */
    bool worker_active() const;

    /** Звучит ли PCM в коде ц прямо сейчас (для пульсации UI). */
    bool is_playing() const;

    /** Прерывает текущее воспроизведение (отменяет следующие фразы). */
    void abort();

    /**
     * Разбор WAV и разворот моно в стерео — отдельно от HTTP и кодека,
     * чтобы их можно было проверить на хосте без ESP-IDF.
     */
    static esp_err_t wav_parse(const uint8_t *wav, size_t len, kouki_speech_wav_t *out);
    static esp_err_t stereo_expand(int16_t *samples, size_t frames);

private:
    /** Отменено ли поколение (0, отменённое или устаревшее). */
    bool is_gen_aborted(uint32_t gen) const;

    /** Колбэк событий HTTP: логирует рукопожатие TLS через Http. */
    esp_err_t on_http_event(esp_http_client_event_t *evt);
    static esp_err_t http_event_entry(esp_http_client_event_t *evt);

    /**
     * Забирает WAV с сервера по чанкам в буфер PSRAM; проверяет отмену
     * поколением между каждым этапом HTTP и между чанками чтения.
     */
    esp_err_t fetch_wav(const char *body, size_t body_len, uint32_t gen, uint8_t **out_buf,
                        size_t *out_len);

    /** Одна фраза: POST /api/speech, разбор WAV, воспроизведение чанками. */
    esp_err_t play_single_phrase(const char *phrase, size_t phrase_len, uint32_t gen,
                                 bool *out_played);

    Config &m_config;
    AudioOut &m_audio;
    Tone &m_tone;

    std::atomic<uint32_t> m_latest_gen{0};
    std::atomic<uint32_t> m_aborted_up_to_gen{0};
    std::atomic<uint32_t> m_active_gen{0};
    std::atomic<uint32_t> m_speaking_gen{0};
    std::atomic<int> m_active_workers{0};
};

/** Единственный экземпляр озвучивания. */
Speech &speech();

}  // namespace kouki

#endif  // APP_SPEECH_HPP
