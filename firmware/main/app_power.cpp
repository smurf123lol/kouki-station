#include "app_power.hpp"

#include <cmath>
#include <cstdio>

#ifdef ESP_PLATFORM
// ST77916.h — вендорный C-заголовок без extern "C", а C++-модулю из него нужна
// только Set_Backlight: объявляем её напрямую как C-функцию, чтобы символ
// слинковался с ST77916.c (иначе получается C++-манглинг).
extern "C" void Set_Backlight(uint8_t Light);
#include "app_voice.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
// Для тестов на хосте объявляем слабую функцию Set_Backlight
__attribute__((weak)) void Set_Backlight(uint8_t Light)
{
    (void)Light;
}
#endif

namespace kouki {
namespace {

#ifdef ESP_PLATFORM
const char *const TAG = "kouki.power";
#endif

}  // namespace

#ifdef ESP_PLATFORM
void Power::lock()
{
    if (m_mutex) {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
    }
}

void Power::unlock()
{
    if (m_mutex) {
        xSemaphoreGive(m_mutex);
    }
}
#endif

void Power::init()
{
#ifdef ESP_PLATFORM
    if (!m_mutex) {
        m_mutex = xSemaphoreCreateMutex();
    }
    lock();
#else
    // На хосте блокировок нет: однопоточный стенд.
#endif
    m_idle_ms = 0;
    m_current_brightness = (float)KOUKI_POWER_BRIGHTNESS_ACTIVE;
    m_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    m_applied_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    Set_Backlight(m_applied_brightness);
#ifdef ESP_PLATFORM
    unlock();

    ESP_LOGI(TAG, "Диспетчер питания инициализирован (100%% яркости)");
#endif
}

void Power::notify_activity()
{
#ifdef ESP_PLATFORM
    lock();
#endif
    m_idle_ms = 0;
    m_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    m_current_brightness = (float)KOUKI_POWER_BRIGHTNESS_ACTIVE;
    uint8_t prev = m_applied_brightness;
    m_applied_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    if (prev != KOUKI_POWER_BRIGHTNESS_ACTIVE) {
        Set_Backlight(KOUKI_POWER_BRIGHTNESS_ACTIVE);
#ifdef ESP_PLATFORM
        ESP_LOGD(TAG, "Активность: яркость восстановлена до 100%%");
#endif
    }
#ifdef ESP_PLATFORM
    unlock();
#endif
}

void Power::tick(uint32_t dt_ms)
{
#ifdef ESP_PLATFORM
    // Проверка состояния FSM выполняется lock-free ДО захвата m_mutex во избежание взаимных блокировок
    if (kouki_voice_get_state() != KOUKI_VOICE_IDLE) {
        notify_activity();
        return;
    }
    lock();
#endif

    // Защита от переполнения uint32 при длительном бездействии (> 49 суток)
    if (m_idle_ms < (KOUKI_POWER_IDLE_OFF_MS + 5000)) {
        m_idle_ms += dt_ms;
    }

    uint32_t fade_duration_ms = KOUKI_POWER_FADE_WAKE_MS;

    if (m_idle_ms >= KOUKI_POWER_IDLE_OFF_MS) {
        m_target_brightness = KOUKI_POWER_BRIGHTNESS_OFF; // 0%
        fade_duration_ms = KOUKI_POWER_FADE_OFF_MS;       // 1000 мс
    } else if (m_idle_ms >= KOUKI_POWER_IDLE_DIM_MS) {
        m_target_brightness = KOUKI_POWER_BRIGHTNESS_DIM; // 20%
        fade_duration_ms = KOUKI_POWER_FADE_DIM_MS;       // 1500 мс
    } else {
        m_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE; // 100%
        fade_duration_ms = KOUKI_POWER_FADE_WAKE_MS;
    }

    if ((int)m_current_brightness != m_target_brightness) {
        float step = ((float)dt_ms / (float)fade_duration_ms) * 100.0f;
        if (step < 0.5f) {
            step = 0.5f;
        }

        if (m_current_brightness > (float)m_target_brightness) {
            m_current_brightness -= step;
            if (m_current_brightness < (float)m_target_brightness) {
                m_current_brightness = (float)m_target_brightness;
            }
        } else {
            m_current_brightness += step;
            if (m_current_brightness > (float)m_target_brightness) {
                m_current_brightness = (float)m_target_brightness;
            }
        }

        uint8_t new_b = (uint8_t)(m_current_brightness + 0.5f);
        if (new_b != m_applied_brightness) {
            m_applied_brightness = new_b;
            Set_Backlight(m_applied_brightness);
        }
    }

#ifdef ESP_PLATFORM
    unlock();
#endif
}

uint8_t Power::brightness()
{
#ifdef ESP_PLATFORM
    lock();
#endif
    uint8_t b = m_applied_brightness;
#ifdef ESP_PLATFORM
    unlock();
#endif
    return b;
}

uint32_t Power::idle_ms()
{
#ifdef ESP_PLATFORM
    lock();
#endif
    uint32_t idle = m_idle_ms;
#ifdef ESP_PLATFORM
    unlock();
#endif
    return idle;
}

Power &power()
{
    static Power s_instance;
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

void kouki_power_init(void)
{
    kouki::power().init();
}

void kouki_power_notify_activity(void)
{
    kouki::power().notify_activity();
}

void kouki_power_tick(uint32_t dt_ms)
{
    kouki::power().tick(dt_ms);
}

uint8_t kouki_power_get_brightness(void)
{
    return kouki::power().brightness();
}

uint32_t kouki_power_get_idle_ms(void)
{
    return kouki::power().idle_ms();
}

}  // extern "C"
