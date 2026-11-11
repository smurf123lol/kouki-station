#include "app_audio_out.hpp"

#include <stdlib.h>
#include <string.h>

#include "esp_board_init.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <chrono>
#endif

namespace kouki {
namespace {

#ifdef ESP_PLATFORM
const char *const TAG = "kouki.audio_out";
#endif

/*
 * 16 кГц стерео PCM16: 10 мс = 160 сэмплов * 2 канала = 320 сэмплов (640 байт).
 * Тишина для продувки DMA — та же, что в C-модуле.
 */
const int16_t kSilenceChunk[320] = {0};

}  // namespace

// ------------------------------------------------------------------ lock ----

void AudioOut::lock()
{
    if (!ready()) {
        (void)init();
    }
#ifdef ESP_PLATFORM
    if (m_mutex) {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
    }
#else
    m_mutex.lock();
#endif
}

void AudioOut::unlock()
{
#ifdef ESP_PLATFORM
    if (m_mutex) {
        xSemaphoreGive(m_mutex);
    }
#else
    m_mutex.unlock();
#endif
}

// ---------------------------------------------------------- очередь/init ---

bool AudioOut::ready()
{
#ifdef ESP_PLATFORM
    return m_mutex != nullptr;
#else
    return m_queue_ready;
#endif
}

bool AudioOut::queue_push(const Item &item, uint32_t timeout_ms)
{
#ifdef ESP_PLATFORM
    return xQueueSend(m_queue, &item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
#else
    std::unique_lock<std::mutex> guard(m_queue_mutex);
    const bool room = m_space_cv.wait_for(guard, std::chrono::milliseconds(timeout_ms),
                                         [this] { return m_count < kQueueLen; });
    if (!room) {
        return false;
    }
    m_items[m_tail] = item;
    m_tail = (m_tail + 1) % kQueueLen;
    m_count++;
    m_queue_cv.notify_one();
    return true;
#endif
}

bool AudioOut::queue_pop(Item &item, bool wait_forever)
{
#ifdef ESP_PLATFORM
    return xQueueReceive(m_queue, &item, wait_forever ? portMAX_DELAY : 0) == pdTRUE;
#else
    std::unique_lock<std::mutex> guard(m_queue_mutex);
    if (wait_forever) {
        m_queue_cv.wait(guard, [this] { return m_count > 0 || m_worker_stop; });
    }
    if (m_count == 0) {
        return false;
    }
    item = m_items[m_head];
    m_head = (m_head + 1) % kQueueLen;
    m_count--;
    m_space_cv.notify_one();
    return true;
#endif
}

uint32_t AudioOut::queue_count()
{
#ifdef ESP_PLATFORM
    return (uint32_t)uxQueueMessagesWaiting(m_queue);
#else
    std::lock_guard<std::mutex> guard(m_queue_mutex);
    return (uint32_t)m_count;
#endif
}

void AudioOut::notify_idle()
{
    if (queue_count() != 0) {
        return;
    }
#ifdef ESP_PLATFORM
    if (m_idle_sem != nullptr) {
        xSemaphoreGive(m_idle_sem);
    }
#else
    (void)0;  // на хосте wait_idle опрашивает очередь и m_playing
#endif
}

esp_err_t AudioOut::init()
{
    if (ready()) {
        return ESP_OK;
    }

#ifdef ESP_PLATFORM
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t idle_sem = xSemaphoreCreateBinary();
    QueueHandle_t queue = xQueueCreate(kQueueLen, sizeof(Item));

    if (mutex == nullptr || idle_sem == nullptr || queue == nullptr) {
        if (mutex != nullptr) vSemaphoreDelete(mutex);
        if (idle_sem != nullptr) vSemaphoreDelete(idle_sem);
        if (queue != nullptr) vQueueDelete(queue);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&m_spinlock);
    if (m_mutex == nullptr) {
        m_mutex = mutex;
        m_idle_sem = idle_sem;
        m_queue = queue;
        mutex = nullptr;
        idle_sem = nullptr;
        queue = nullptr;
    }
    taskEXIT_CRITICAL(&m_spinlock);

    if (mutex != nullptr) vSemaphoreDelete(mutex);
    if (idle_sem != nullptr) vSemaphoreDelete(idle_sem);
    if (queue != nullptr) vQueueDelete(queue);

    if (m_task == nullptr) {
        BaseType_t ret = xTaskCreate(worker_entry, "audio.out", 4096, this, 6, &m_task);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "не удалось создать задачу audio.out");
            return ESP_FAIL;
        }
    }
#else
    {
        std::lock_guard<std::mutex> guard(m_queue_mutex);
        m_queue_ready = true;
        m_worker_stop = false;
        m_head = 0;
        m_tail = 0;
        m_count = 0;
    }
    m_thread = std::thread(&AudioOut::worker_loop, this);
#endif
    return ESP_OK;
}

#ifndef ESP_PLATFORM
AudioOut::~AudioOut()
{
    stop_worker();
}

void AudioOut::stop_worker()
{
    {
        std::lock_guard<std::mutex> guard(m_queue_mutex);
        if (!m_queue_ready) {
            return;
        }
        m_worker_stop = true;
    }
    m_queue_cv.notify_all();
    m_space_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AudioOut::delay_ms(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

uint32_t AudioOut::now_ms()
{
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

// ---------------------------------------------------------------- worker ---

#ifdef ESP_PLATFORM
void AudioOut::worker_entry(void *arg)
{
    static_cast<AudioOut *>(arg)->worker_loop();
}
#endif

void AudioOut::worker_loop()
{
    for (;;) {
        Item item;
        if (!queue_pop(item, true)) {
#ifndef ESP_PLATFORM
            if (m_worker_stop) {
                break;
            }
#endif
            continue;
        }
#ifndef ESP_PLATFORM
        if (m_worker_stop) {
            break;
        }
#endif

        if (m_abort.load(std::memory_order_acquire)) {
            if (item.dynamically_allocated && item.data != nullptr) {
                free(item.data);
            }
            continue;
        }

        m_playing.store(true, std::memory_order_release);

        if (item.type == ItemType::pcm && item.data != nullptr && item.bytes > 0) {
            lock();
            const size_t total_bytes = item.bytes;
            size_t sent = 0;

            while (sent < total_bytes) {
                if (m_abort.load(std::memory_order_acquire)) {
                    break;
                }
                const size_t rem = total_bytes - sent;
                const size_t chunk_bytes = rem < 3200 ? rem : 3200; /* ~25 мс чанк */
                esp_err_t err = esp_audio_play(
                    (const int16_t *)((const uint8_t *)item.data + sent), (int)chunk_bytes,
                    portMAX_DELAY);
                if (err != ESP_OK) {
                    break;
                }
                sent += chunk_bytes;
            }
            unlock();

            if (item.dynamically_allocated && item.data != nullptr) {
                free(item.data);
                item.data = nullptr;
            }
        } else if (item.type == ItemType::silence && item.silence_ms > 0) {
            lock();
            uint32_t remaining_ms = item.silence_ms;
            while (remaining_ms > 0) {
                if (m_abort.load(std::memory_order_acquire)) {
                    break;
                }
                const uint32_t step_ms = remaining_ms < 10 ? remaining_ms : 10;
                const int step_bytes = (int)(step_ms * 64);
                esp_err_t err = esp_audio_play(kSilenceChunk, step_bytes, portMAX_DELAY);
                if (err != ESP_OK) {
                    break;
                }
                remaining_ms -= step_ms;
            }
            unlock();
        }

        m_playing.store(false, std::memory_order_release);

        notify_idle();
    }
}

// ---------------------------------------------------------------- очередь ---

esp_err_t AudioOut::enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership)
{
    if (data == nullptr || bytes == 0) {
        if (transfer_ownership && data != nullptr) {
            free(data);
        }
        return ESP_OK;
    }
    if (!ready()) {
        esp_err_t err = init();
        if (err != ESP_OK) return err;
    }

    m_abort.store(false, std::memory_order_release);

    Item item;
    item.type = ItemType::pcm;
    item.data = data;
    item.bytes = bytes;
    item.dynamically_allocated = transfer_ownership;
    item.silence_ms = 0;

    if (!queue_push(item, 1000)) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "очередь audio.out переполнена");
#endif
        if (transfer_ownership && data != nullptr) {
            free(data);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t AudioOut::enqueue_silence(uint32_t silence_ms)
{
    if (silence_ms == 0) {
        return ESP_OK;
    }
    if (!ready()) {
        esp_err_t err = init();
        if (err != ESP_OK) return err;
    }

    m_abort.store(false, std::memory_order_release);

    Item item;
    item.type = ItemType::silence;
    item.data = nullptr;
    item.bytes = 0;
    item.dynamically_allocated = false;
    item.silence_ms = silence_ms;

    if (!queue_push(item, 1000)) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "очередь audio.out (silence) переполнена");
#endif
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void AudioOut::abort()
{
    m_abort.store(true, std::memory_order_release);

    if (ready()) {
        Item item;
        while (queue_pop(item, false)) {
            if (item.dynamically_allocated && item.data != nullptr) {
                free(item.data);
            }
        }
    }

    /* Гарантированная продувка DMA тишиной при прерывании */
    lock();
    esp_audio_play(kSilenceChunk, (int)sizeof(kSilenceChunk), portMAX_DELAY);
    unlock();
}

esp_err_t AudioOut::wait_idle(TickType_t ticks_to_wait)
{
    if (!ready()) {
        return ESP_OK;
    }

#ifdef ESP_PLATFORM
    TickType_t start = xTaskGetTickCount();
#else
    const uint32_t start = now_ms();
#endif
    while (queue_count() > 0 || m_playing.load(std::memory_order_acquire)) {
#ifdef ESP_PLATFORM
        if (ticks_to_wait != portMAX_DELAY &&
            (xTaskGetTickCount() - start) > ticks_to_wait) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
#else
        if (ticks_to_wait != portMAX_DELAY && (now_ms() - start) > ticks_to_wait) {
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(1);
#endif
    }
    return ESP_OK;
}

bool AudioOut::is_busy()
{
    if (!ready()) {
        return false;
    }
    return queue_count() > 0 || m_playing.load(std::memory_order_acquire);
}

// --------------------------------------------------------- прямое аудио ----

esp_err_t AudioOut::play(const int16_t *data, int bytes, TickType_t ticks_to_wait)
{
    if (data == nullptr || bytes <= 0) {
        return ESP_OK;
    }
    lock();
    esp_err_t err = esp_audio_play(data, bytes, ticks_to_wait);
    unlock();
    return err;
}

esp_err_t AudioOut::play_silence(uint32_t silence_ms, TickType_t ticks_to_wait)
{
    if (silence_ms == 0) {
        return ESP_OK;
    }

    lock();
    esp_err_t err = ESP_OK;
    uint32_t remaining_ms = silence_ms;

    while (remaining_ms > 0) {
        uint32_t step_ms = remaining_ms < 10 ? remaining_ms : 10;
        int step_bytes = (int)(step_ms * 64);
        err = esp_audio_play(kSilenceChunk, step_bytes, ticks_to_wait);
        if (err != ESP_OK) {
            break;
        }
        remaining_ms -= step_ms;
    }
    unlock();
    return err;
}

esp_err_t AudioOut::play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms,
                                   TickType_t ticks_to_wait)
{
    esp_err_t err = ESP_OK;
    if (data != nullptr && bytes > 0) {
        err = play(data, bytes, ticks_to_wait);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (tail_ms > 0) {
        err = play_silence(tail_ms, ticks_to_wait);
    }
    return err;
}

AudioOut &audio_out()
{
    static AudioOut s_instance;
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

esp_err_t kouki_audio_out_init(void)
{
    return kouki::audio_out().init();
}

void kouki_audio_out_lock(void)
{
    kouki::audio_out().lock();
}

void kouki_audio_out_unlock(void)
{
    kouki::audio_out().unlock();
}

esp_err_t kouki_audio_out_enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership)
{
    return kouki::audio_out().enqueue_pcm(data, bytes, transfer_ownership);
}

esp_err_t kouki_audio_out_enqueue_silence(uint32_t silence_ms)
{
    return kouki::audio_out().enqueue_silence(silence_ms);
}

void kouki_audio_out_abort(void)
{
    kouki::audio_out().abort();
}

esp_err_t kouki_audio_out_wait_idle(TickType_t ticks_to_wait)
{
    return kouki::audio_out().wait_idle(ticks_to_wait);
}

bool kouki_audio_out_is_busy(void)
{
    return kouki::audio_out().is_busy();
}

esp_err_t kouki_audio_play(const int16_t *data, int bytes, TickType_t ticks_to_wait)
{
    return kouki::audio_out().play(data, bytes, ticks_to_wait);
}

esp_err_t kouki_audio_play_silence(uint32_t silence_ms, TickType_t ticks_to_wait)
{
    return kouki::audio_out().play_silence(silence_ms, ticks_to_wait);
}

esp_err_t kouki_audio_play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms,
                                     TickType_t ticks_to_wait)
{
    return kouki::audio_out().play_with_tail(data, bytes, tail_ms, ticks_to_wait);
}

}  // extern "C"
