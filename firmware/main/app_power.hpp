#ifndef APP_POWER_HPP
#define APP_POWER_HPP

#include <cstdint>

// C-совместимый заголовок фасада: константы KOUKI_POWER_* остаются общими
// для C- и C++-модулей.
#include "app_power.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace kouki {

/**
 * Диспетчер питания и подсветки (T121 П3).
 *
 * Владеет состоянием модуля: время неактивности, текущая/целевая/применённая
 * яркость и мьютекс. Мьютекс создаётся в init() до первого использования.
 * Экземпляр один (см. kouki::power()), init() зовётся из app_main.
 */
class Power {
public:
    Power() = default;
    Power(const Power &) = delete;
    Power &operator=(const Power &) = delete;

    /** Создаёт мьютекс, сбрасывает яркость на 100% и применяет её. */
    void init();

    /** Пользователь активен: idle = 0, яркость сразу 100%. */
    void notify_activity();

    /** Периодический шаг: копит idle и ведёт плавную яркость. */
    void tick(uint32_t dt_ms);

    /** Текущая применённая яркость (0..100). */
    uint8_t brightness();

    /** Время неактивности в миллисекундах. */
    uint32_t idle_ms();

private:
#ifdef ESP_PLATFORM
    void lock();
    void unlock();
#endif

    uint32_t m_idle_ms = 0;
    float m_current_brightness = (float)KOUKI_POWER_BRIGHTNESS_ACTIVE;
    uint8_t m_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    uint8_t m_applied_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
#ifdef ESP_PLATFORM
    SemaphoreHandle_t m_mutex = nullptr;
#endif
};

/** Единственный экземпляр диспетчера питания. */
Power &power();

}  // namespace kouki

#endif  // APP_POWER_HPP
