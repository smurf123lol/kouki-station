#ifndef APP_AUDIO_OUT_HPP
#define APP_AUDIO_OUT_HPP

#include <stddef.h>
#include <stdint.h>

#include <atomic>

// C-совместимый заголовок фасада: типы esp_err_t/TickType_t и порт MAX_DELAY
// остаются общими для C- и C++-модулей.
#include "app_audio_out.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace kouki {

/**
 * Единый владелец TX-кодека (T121 П5): очередь, фоновый worker и leaf-lock.
 *
 * Владеет состоянием модуля (мьютекс, спинлок, очередь, задача, семафор
 * простоя) как членами; всё создаётся в init() до первого использования.
 * Экземпляр один (см. kouki::audio_out()), init() зовётся из app_main.
 *
 * Иерархия блокировок прежняя: VoiceStateManager::m_lock (Level 1) ->
 * AudioOut::lock() (Level 2). Внутри critical section audio_out никогда не
 * берутся мьютексы FSM или wake-word.
 *
 * На хосте (без ESP-IDF) те же методы работают на std::thread/мьютексе, чтобы
 * host-стенд проверял настоящую очередь и порядок воспроизведения, а не
 * заглушку. Поток создаётся в init() и останавливается деструктором.
 */
class AudioOut {
public:
    /** Сколько элементов очередь держит до переполнения (16, как в C-модуле). */
    static constexpr size_t kQueueLen = 16;

    AudioOut() = default;
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;
#ifndef ESP_PLATFORM
    ~AudioOut();
#endif

    /** Создаёт мьютекс, семафор простоя, очередь и запускает worker. */
    esp_err_t init();

    /** Захватывает монопольный доступ к TX кодеку (leaf-lock). */
    void lock();

    /** Освобождает доступ к TX кодеку. */
    void unlock();

    /** Ставит PCM16 стерео-сэмплы в очередь; владение данными — по флагу. */
    esp_err_t enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership);

    /** Ставит интервал тишины в очередь (продувка DMA нулями). */
    esp_err_t enqueue_silence(uint32_t silence_ms);

    /** Мгновенно прерывает воспроизведение, сбрасывает очередь и глушит DMA. */
    void abort();

    /** Ждёт, пока очередь опустеет и кодек замолчит. */
    esp_err_t wait_idle(TickType_t ticks_to_wait);

    /** true, если в очереди есть элементы или кодек ещё играет. */
    bool is_busy();

    /** Синхронное воспроизведение под leaf-lock. */
    esp_err_t play(const int16_t *data, int bytes, TickType_t ticks_to_wait);

    /** Синхронная продувка DMA тишиной под leaf-lock. */
    esp_err_t play_silence(uint32_t silence_ms, TickType_t ticks_to_wait);

    /** Синхронное воспроизведение с завершающим хвостом тишины. */
    esp_err_t play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms,
                             TickType_t ticks_to_wait);

private:
    enum class ItemType {
        pcm,
        silence,
    };

    struct Item {
        ItemType type = ItemType::pcm;
        int16_t *data = nullptr;
        size_t bytes = 0;
        bool dynamically_allocated = false;
        uint32_t silence_ms = 0;
    };

    /** Тело worker'а: вынимает элементы и проигрывает их по одному. */
    void worker_loop();

    /** Кладёт элемент в очередь; false — очередь не приняла за timeout_ms. */
    bool queue_push(const Item &item, uint32_t timeout_ms);

    /** Вынимает элемент; блокирующий режим — до появления элемента. */
    bool queue_pop(Item &item, bool wait_forever);

    /** Сколько элементов сейчас в очереди. */
    uint32_t queue_count();

    /** Готовы ли очередь и мьютекс (init() уже вызывался). */
    bool ready();

    /** Отдаёт семафор простоя, если очередь пуста. */
    void notify_idle();

#ifdef ESP_PLATFORM
    /** Точка входа FreeRTOS-задачи: arg — экземпляр AudioOut. */
    static void worker_entry(void *arg);
#else
    void stop_worker();
    void delay_ms(uint32_t ms);
    uint32_t now_ms();
#endif

    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_playing{false};

#ifdef ESP_PLATFORM
    SemaphoreHandle_t m_mutex = nullptr;
    portMUX_TYPE m_spinlock = portMUX_INITIALIZER_UNLOCKED;
    QueueHandle_t m_queue = nullptr;
    TaskHandle_t m_task = nullptr;
    SemaphoreHandle_t m_idle_sem = nullptr;
#else
    std::mutex m_mutex;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::condition_variable m_space_cv;
    std::thread m_thread;
    Item m_items[kQueueLen];
    size_t m_head = 0;
    size_t m_tail = 0;
    size_t m_count = 0;
    bool m_queue_ready = false;
    std::atomic<bool> m_worker_stop{false};
#endif
};

/** Единственный владелец аудиовыхода. */
AudioOut &audio_out();

}  // namespace kouki

#endif  // APP_AUDIO_OUT_HPP
