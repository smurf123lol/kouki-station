#include "app_history.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_time.hpp"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace kouki {
namespace {

#ifdef ESP_PLATFORM
const char *const TAG = "kouki.history";
#endif

}  // namespace

void History::init()
{
    if (m_items != nullptr) {
        return; // Уже инициализирован
    }

#ifdef ESP_PLATFORM
    m_mutex = xSemaphoreCreateMutex();
    // Выделяем кольцевой буфер истории в PSRAM (SPI RAM)
    m_items = (kouki_history_item_t *)heap_caps_malloc(
        sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m_items) {
        ESP_LOGW(TAG, "Не удалось выделить историю в PSRAM, откат на внутренний heap");
        m_items = (kouki_history_item_t *)malloc(sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY);
    }
    if (m_items) {
        memset(m_items, 0, sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY);
        ESP_LOGI(TAG, "Буфер истории инициализирован (емкость: %d, размер: %u байт)",
                 KOUKI_HISTORY_CAPACITY, (unsigned)(sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY));
    } else {
        ESP_LOGE(TAG, "Критическая ошибка: не удалось выделить память под историю!");
    }
#else
    m_items = (kouki_history_item_t *)calloc(KOUKI_HISTORY_CAPACITY, sizeof(kouki_history_item_t));
#endif

    m_head = 0;
    m_count = 0;
    m_version.store(0, std::memory_order_release);
}

#ifdef ESP_PLATFORM
void History::lock()
{
    if (m_mutex) {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
    }
}

void History::unlock()
{
    if (m_mutex) {
        xSemaphoreGive(m_mutex);
    }
}
#endif

void History::push(const kouki_voice_result_t *res)
{
    if (!res || !m_items) {
        return;
    }

    // Пропускаем пустые или ошибочные результаты без транскрипта и ответа
    if (res->transcript[0] == '\0' && res->answer[0] == '\0') {
        return;
    }

    char hm[16] = {0};
    kouki::timekeeper().format_hm(hm, sizeof(hm));

    const bool is_task = (strcmp(res->outcome, "created") == 0 ||
                          res->task_title[0] != '\0' ||
                          strstr(res->intent, "task") != nullptr);
    const bool is_answer = (res->answer[0] != '\0' || strcmp(res->intent, "ask") == 0);

#ifdef ESP_PLATFORM
    lock();
#endif

    kouki_history_item_t *slot = &m_items[m_head];
    memset(slot, 0, sizeof(*slot));

    strncpy(slot->transcript, res->transcript, sizeof(slot->transcript) - 1);
    strncpy(slot->title, res->task_title, sizeof(slot->title) - 1);
    strncpy(slot->answer, res->answer, sizeof(slot->answer) - 1);
    strncpy(slot->outcome, res->outcome, sizeof(slot->outcome) - 1);
    strncpy(slot->timestamp_hm, hm, sizeof(slot->timestamp_hm) - 1);
    slot->is_task = is_task;
    slot->is_answer = is_answer;

    m_head = (m_head + 1) % KOUKI_HISTORY_CAPACITY;
    if (m_count < KOUKI_HISTORY_CAPACITY) {
        m_count++;
    }
    m_version.fetch_add(1, std::memory_order_release);

#ifdef ESP_PLATFORM
    const size_t total = m_count;
    char preview[64];
    const char *src = slot->transcript[0] ? slot->transcript : slot->answer;
    size_t cut = 0;
    const size_t max_bytes = sizeof(preview) - 1;
    while (src[cut] != '\0' && cut < max_bytes) {
        cut++;
    }
    while (cut > 0 && (((unsigned char)src[cut] & 0xC0u) == 0x80u)) {
        cut--;
    }
    memcpy(preview, src, cut);
    preview[cut] = '\0';

    unlock();
    ESP_LOGI(TAG, "Добавлена запись в историю (всего: %u): '%s'",
             (unsigned)total, preview);
#endif
}

size_t History::count()
{
    size_t count = 0;
#ifdef ESP_PLATFORM
    lock();
#endif
    count = m_count;
#ifdef ESP_PLATFORM
    unlock();
#endif
    return count;
}

uint32_t History::version()
{
    return m_version.load(std::memory_order_acquire);
}

bool History::get(size_t index, kouki_history_item_t *out_item)
{
    if (!out_item || !m_items) {
        return false;
    }

#ifdef ESP_PLATFORM
    lock();
#endif

    if (index >= m_count) {
#ifdef ESP_PLATFORM
        unlock();
#endif
        return false;
    }

    // index 0 — самый свежий (последний записанный перед m_head)
    // index 1 — предпоследний, и т.д.
    size_t pos = (m_head + KOUKI_HISTORY_CAPACITY - 1 - index) % KOUKI_HISTORY_CAPACITY;
    *out_item = m_items[pos];


#ifdef ESP_PLATFORM
    unlock();
#endif
    return true;
}

void History::clear()
{
#ifdef ESP_PLATFORM
    lock();
#endif

    m_head = 0;
    m_count = 0;
    if (m_items) {
        memset(m_items, 0, sizeof(kouki_history_item_t) * KOUKI_HISTORY_CAPACITY);
    }
    m_version.fetch_add(1, std::memory_order_release);

#ifdef ESP_PLATFORM
    unlock();
#endif
}

History &history()
{
    static History s_instance;
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

void kouki_history_init(void)
{
    kouki::history().init();
}

void kouki_history_push(const kouki_voice_result_t *res)
{
    kouki::history().push(res);
}

size_t kouki_history_count(void)
{
    return kouki::history().count();
}

uint32_t kouki_history_version(void)
{
    return kouki::history().version();
}

bool kouki_history_get(size_t index, kouki_history_item_t *out_item)
{
    return kouki::history().get(index, out_item);
}

void kouki_history_clear(void)
{
    kouki::history().clear();
}

}  // extern "C"
