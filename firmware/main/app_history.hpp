#ifndef APP_HISTORY_HPP
#define APP_HISTORY_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: kouki_history_item_t и KOUKI_HISTORY_CAPACITY
// остаются общими для C- и C++-модулей.
#include "app_history.h"
#include "app_voice.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace kouki {

/**
 * Кольцевой буфер истории распознаваний (T121 П3).
 *
 * Владеет состоянием модуля: буфер записей (в PSRAM, при неудаче — внутренний
 * heap), позиция записи, число валидных записей, монотонный счётчик версии и
 * мьютекс. Буфер выделяется один раз в init(); удаления и деструктора нет.
 * Экземпляр один (см. kouki::history()), init() зовётся из app_main.
 */
class History {
public:
    History() = default;
    History(const History &) = delete;
    History &operator=(const History &) = delete;

    /** Выделяет буфер и создаёт мьютекс; повторный вызов — no-op. */
    void init();

    /** Кладёт результат в кольцо, вытесняя самую старую запись. */
    void push(const kouki_voice_result_t *res);

    /** Текущее число записей (0..KOUKI_HISTORY_CAPACITY). */
    size_t count();

    /** Монотонный счётчик изменений (для O(1)-проверки в UI). */
    uint32_t version();

    /** Копирует запись: index 0 — самая свежая. false — индекс вне диапазона. */
    bool get(size_t index, kouki_history_item_t *out_item);

    /** Очищает кольцо; версия увеличивается на 1. */
    void clear();

private:
#ifdef ESP_PLATFORM
    void lock();
    void unlock();
#endif

    kouki_history_item_t *m_items = nullptr;
    size_t m_head = 0;   // Следующая позиция для записи
    size_t m_count = 0;  // Текущее число элементов (0..KOUKI_HISTORY_CAPACITY)
    std::atomic<uint32_t> m_version{0};
#ifdef ESP_PLATFORM
    SemaphoreHandle_t m_mutex = nullptr;
#endif
};

/** Единственный экземпляр истории. */
History &history();

}  // namespace kouki

#endif  // APP_HISTORY_HPP
