#include "app_history.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "app_time.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "kouki.history";
static SemaphoreHandle_t s_history_mutex = NULL;
#endif

static kouki_history_item_t *s_items = NULL;
static size_t s_head = 0;   // Следующая позиция для записи
static size_t s_count = 0;  // Текущее число элементов (0..KOUKI_HISTORY_CAPACITY)

void kouki_history_init(void)
{
    if (s_items != NULL) {
        return; // Уже инициализирован
    }

#ifdef ESP_PLATFORM
    s_history_mutex = xSemaphoreCreateMutex();
    // Выделяем кольцевой буфер истории в PSRAM (SPI RAM)
    s_items = (kouki_history_item_t *)heap_caps_malloc(
        sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!s_items) {
        ESP_LOGW(TAG, "Не удалось выделить историю в PSRAM, откат на внутренний heap");
        s_items = (kouki_history_item_t *)malloc(sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY);
    }
    if (s_items) {
        memset(s_items, 0, sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY);
        ESP_LOGI(TAG, "Буфер истории инициализирован (емкость: %d, размер: %u байт)",
                 KOUKI_HISTORY_CAPACITY, (unsigned)(sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY));
    } else {
        ESP_LOGE(TAG, "Критическая ошибка: не удалось выделить память под историю!");
    }
#else
    s_items = (kouki_history_item_t *)calloc(KOUKI_HISTORY_CAPACITY, sizeof(kouki_history_item_t));
#endif

    s_head = 0;
    s_count = 0;
}

void kouki_history_push(const kouki_voice_result_t *res)
{
    if (!res || !s_items) {
        return;
    }

    // Пропускаем пустые или ошибочные результаты без транскрипта и ответа
    if (res->transcript[0] == '\0' && res->answer[0] == '\0') {
        return;
    }

    kouki_history_item_t item;
    memset(&item, 0, sizeof(item));

    strncpy(item.transcript, res->transcript, sizeof(item.transcript) - 1);
    strncpy(item.title, res->task_title, sizeof(item.title) - 1);
    strncpy(item.answer, res->answer, sizeof(item.answer) - 1);
    strncpy(item.outcome, res->outcome, sizeof(item.outcome) - 1);

    kouki_time_format_hm(item.timestamp_hm, sizeof(item.timestamp_hm));

    if (strcmp(res->outcome, "created") == 0 || res->task_title[0] != '\0' || strstr(res->intent, "task") != NULL) {
        item.is_task = true;
    }
    if (res->answer[0] != '\0' || strcmp(res->intent, "ask") == 0) {
        item.is_answer = true;
    }

#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    }
#endif

    s_items[s_head] = item;
    s_head = (s_head + 1) % KOUKI_HISTORY_CAPACITY;
    if (s_count < KOUKI_HISTORY_CAPACITY) {
        s_count++;
    }

#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreGive(s_history_mutex);
    }
    ESP_LOGI(TAG, "Добавлена запись в историю (всего: %u): '%s'",
             (unsigned)s_count, item.transcript[0] ? item.transcript : item.answer);
#endif
}

size_t kouki_history_count(void)
{
    size_t count = 0;
#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    }
#endif
    count = s_count;
#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreGive(s_history_mutex);
    }
#endif
    return count;
}

bool kouki_history_get(size_t index, kouki_history_item_t *out_item)
{
    if (!out_item || !s_items) {
        return false;
    }

#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    }
#endif

    if (index >= s_count) {
#ifdef ESP_PLATFORM
        if (s_history_mutex) {
            xSemaphoreGive(s_history_mutex);
        }
#endif
        return false;
    }

    // index 0 — самый свежий (последний записанный перед s_head)
    // index 1 — предпоследний, и т.д.
    size_t pos = (s_head + KOUKI_HISTORY_CAPACITY - 1 - index) % KOUKI_HISTORY_CAPACITY;
    *out_item = s_items[pos];

#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreGive(s_history_mutex);
    }
#endif
    return true;
}

void kouki_history_clear(void)
{
#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreTake(s_history_mutex, portMAX_DELAY);
    }
#endif

    s_head = 0;
    s_count = 0;
    if (s_items) {
        memset(s_items, 0, sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY);
    }

#ifdef ESP_PLATFORM
    if (s_history_mutex) {
        xSemaphoreGive(s_history_mutex);
    }
#endif
}
