#include "app_audio_out.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

typedef enum {
    ITEM_PCM,
    ITEM_SILENCE,
} audio_item_type_t;

typedef struct {
    audio_item_type_t type;
    int16_t *data;
    size_t bytes;
    bool dynamically_allocated;
    uint32_t silence_ms;
} audio_item_t;

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_board_init.h"
#include "esp_log.h"

static const char *TAG = "kouki.audio_out";

#define AUDIO_QUEUE_LEN 16

static SemaphoreHandle_t s_audio_out_mutex = NULL;
static portMUX_TYPE s_audio_out_spinlock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_audio_queue = NULL;
static TaskHandle_t s_audio_task = NULL;
static SemaphoreHandle_t s_idle_sem = NULL;

static atomic_bool s_abort_flag = false;
static atomic_bool s_worker_playing = false;

/* 16 кГц стерео PCM16: 10 мс = 160 сэмплов * 2 канала = 320 сэмплов (640 байт) */
static const int16_t s_silence_chunk[320] = { 0 };

static void audio_out_worker_task(void *arg)
{
    (void)arg;
    audio_item_t item;

    while (1) {
        if (xQueueReceive(s_audio_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (atomic_load_explicit(&s_abort_flag, memory_order_acquire)) {
            if (item.dynamically_allocated && item.data != NULL) {
                free(item.data);
            }
            continue;
        }

        atomic_store_explicit(&s_worker_playing, true, memory_order_release);

        if (item.type == ITEM_PCM && item.data != NULL && item.bytes > 0) {
            kouki_audio_out_lock();
            const size_t total_bytes = item.bytes;
            size_t sent = 0;

            while (sent < total_bytes) {
                if (atomic_load_explicit(&s_abort_flag, memory_order_acquire)) {
                    break;
                }
                const size_t rem = total_bytes - sent;
                const size_t chunk_bytes = rem < 3200 ? rem : 3200; /* ~25 мс чанк */
                esp_err_t err = esp_audio_play((const int16_t *)((const uint8_t *)item.data + sent),
                                               (int)chunk_bytes, portMAX_DELAY);
                if (err != ESP_OK) {
                    break;
                }
                sent += chunk_bytes;
            }
            kouki_audio_out_unlock();

            if (item.dynamically_allocated && item.data != NULL) {
                free(item.data);
                item.data = NULL;
            }
        } else if (item.type == ITEM_SILENCE && item.silence_ms > 0) {
            kouki_audio_out_lock();
            uint32_t remaining_ms = item.silence_ms;
            while (remaining_ms > 0) {
                if (atomic_load_explicit(&s_abort_flag, memory_order_acquire)) {
                    break;
                }
                const uint32_t step_ms = remaining_ms < 10 ? remaining_ms : 10;
                const int step_bytes = (int)(step_ms * 64);
                esp_err_t err = esp_audio_play(s_silence_chunk, step_bytes, portMAX_DELAY);
                if (err != ESP_OK) {
                    break;
                }
                remaining_ms -= step_ms;
            }
            kouki_audio_out_unlock();
        }

        atomic_store_explicit(&s_worker_playing, false, memory_order_release);

        if (uxQueueMessagesWaiting(s_audio_queue) == 0 && s_idle_sem != NULL) {
            xSemaphoreGive(s_idle_sem);
        }
    }
}

esp_err_t kouki_audio_out_init(void)
{
    if (s_audio_out_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t idle_sem = xSemaphoreCreateBinary();
    QueueHandle_t queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(audio_item_t));

    if (mutex == NULL || idle_sem == NULL || queue == NULL) {
        if (mutex != NULL) vSemaphoreDelete(mutex);
        if (idle_sem != NULL) vSemaphoreDelete(idle_sem);
        if (queue != NULL) vQueueDelete(queue);
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_audio_out_spinlock);
    if (s_audio_out_mutex == NULL) {
        s_audio_out_mutex = mutex;
        s_idle_sem = idle_sem;
        s_audio_queue = queue;
        mutex = NULL;
        idle_sem = NULL;
        queue = NULL;
    }
    taskEXIT_CRITICAL(&s_audio_out_spinlock);

    if (mutex != NULL) vSemaphoreDelete(mutex);
    if (idle_sem != NULL) vSemaphoreDelete(idle_sem);
    if (queue != NULL) vQueueDelete(queue);

    if (s_audio_task == NULL) {
        BaseType_t ret = xTaskCreate(audio_out_worker_task, "audio.out", 4096, NULL, 6, &s_audio_task);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "не удалось создать задачу audio.out");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void kouki_audio_out_lock(void)
{
    if (s_audio_out_mutex == NULL) {
        (void)kouki_audio_out_init();
    }
    if (s_audio_out_mutex != NULL) {
        xSemaphoreTake(s_audio_out_mutex, portMAX_DELAY);
    }
}

void kouki_audio_out_unlock(void)
{
    if (s_audio_out_mutex != NULL) {
        xSemaphoreGive(s_audio_out_mutex);
    }
}

esp_err_t kouki_audio_out_enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership)
{
    if (data == NULL || bytes == 0) {
        if (transfer_ownership && data != NULL) {
            free(data);
        }
        return ESP_OK;
    }
    if (s_audio_queue == NULL) {
        esp_err_t err = kouki_audio_out_init();
        if (err != ESP_OK) return err;
    }

    atomic_store_explicit(&s_abort_flag, false, memory_order_release);

    audio_item_t item;
    item.type = ITEM_PCM;
    item.data = data;
    item.bytes = bytes;
    item.dynamically_allocated = transfer_ownership;
    item.silence_ms = 0;

    if (xQueueSend(s_audio_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "очередь audio.out переполнена");
        if (transfer_ownership && data != NULL) {
            free(data);
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t kouki_audio_out_enqueue_silence(uint32_t silence_ms)
{
    if (silence_ms == 0) {
        return ESP_OK;
    }
    if (s_audio_queue == NULL) {
        esp_err_t err = kouki_audio_out_init();
        if (err != ESP_OK) return err;
    }

    atomic_store_explicit(&s_abort_flag, false, memory_order_release);

    audio_item_t item;
    item.type = ITEM_SILENCE;
    item.data = NULL;
    item.bytes = 0;
    item.dynamically_allocated = false;
    item.silence_ms = silence_ms;

    if (xQueueSend(s_audio_queue, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "очередь audio.out (silence) переполнена");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void kouki_audio_out_abort(void)
{
    atomic_store_explicit(&s_abort_flag, true, memory_order_release);

    if (s_audio_queue != NULL) {
        audio_item_t item;
        while (xQueueReceive(s_audio_queue, &item, 0) == pdTRUE) {
            if (item.dynamically_allocated && item.data != NULL) {
                free(item.data);
            }
        }
    }

    /* Гарантированная продувка DMA тишиной при прерывании */
    kouki_audio_out_lock();
    esp_audio_play(s_silence_chunk, (int)sizeof(s_silence_chunk), portMAX_DELAY);
    kouki_audio_out_unlock();
}

esp_err_t kouki_audio_out_wait_idle(TickType_t ticks_to_wait)
{
    if (s_audio_queue == NULL) {
        return ESP_OK;
    }

    TickType_t start = xTaskGetTickCount();
    while (uxQueueMessagesWaiting(s_audio_queue) > 0 || atomic_load_explicit(&s_worker_playing, memory_order_acquire)) {
        if (ticks_to_wait != portMAX_DELAY && (xTaskGetTickCount() - start) > ticks_to_wait) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

bool kouki_audio_out_is_busy(void)
{
    if (s_audio_queue == NULL) {
        return false;
    }
    return (uxQueueMessagesWaiting(s_audio_queue) > 0 ||
            atomic_load_explicit(&s_worker_playing, memory_order_acquire));
}

esp_err_t kouki_audio_play(const int16_t *data, int bytes, TickType_t ticks_to_wait)
{
    if (data == NULL || bytes <= 0) {
        return ESP_OK;
    }
    kouki_audio_out_lock();
    esp_err_t err = esp_audio_play(data, bytes, ticks_to_wait);
    kouki_audio_out_unlock();
    return err;
}

esp_err_t kouki_audio_play_silence(uint32_t silence_ms, TickType_t ticks_to_wait)
{
    if (silence_ms == 0) {
        return ESP_OK;
    }

    kouki_audio_out_lock();
    esp_err_t err = ESP_OK;
    uint32_t remaining_ms = silence_ms;

    while (remaining_ms > 0) {
        uint32_t step_ms = remaining_ms < 10 ? remaining_ms : 10;
        int step_bytes = (int)(step_ms * 64);
        err = esp_audio_play(s_silence_chunk, step_bytes, ticks_to_wait);
        if (err != ESP_OK) {
            break;
        }
        remaining_ms -= step_ms;
    }
    kouki_audio_out_unlock();
    return err;
}

esp_err_t kouki_audio_play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms, TickType_t ticks_to_wait)
{
    esp_err_t err = ESP_OK;
    if (data != NULL && bytes > 0) {
        err = kouki_audio_play(data, bytes, ticks_to_wait);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (tail_ms > 0) {
        err = kouki_audio_play_silence(tail_ms, ticks_to_wait);
    }
    return err;
}

#else

/* Хост-стенды и эмуляция без ESP-IDF */
esp_err_t kouki_audio_out_init(void) { return ESP_OK; }
void kouki_audio_out_lock(void) {}
void kouki_audio_out_unlock(void) {}
esp_err_t kouki_audio_out_enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership)
{
    if (transfer_ownership && data != NULL) free(data);
    (void)bytes;
    return ESP_OK;
}
esp_err_t kouki_audio_out_enqueue_silence(uint32_t silence_ms) { (void)silence_ms; return ESP_OK; }
void kouki_audio_out_abort(void) {}
esp_err_t kouki_audio_out_wait_idle(TickType_t ticks_to_wait) { (void)ticks_to_wait; return ESP_OK; }
bool kouki_audio_out_is_busy(void) { return false; }

esp_err_t kouki_audio_play(const int16_t *data, int bytes, TickType_t ticks_to_wait)
{
    (void)data;
    (void)bytes;
    (void)ticks_to_wait;
    return ESP_OK;
}

esp_err_t kouki_audio_play_silence(uint32_t silence_ms, TickType_t ticks_to_wait)
{
    (void)silence_ms;
    (void)ticks_to_wait;
    return ESP_OK;
}

esp_err_t kouki_audio_play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms, TickType_t ticks_to_wait)
{
    (void)data;
    (void)bytes;
    (void)tail_ms;
    (void)ticks_to_wait;
    return ESP_OK;
}

#endif
