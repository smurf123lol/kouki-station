#include "app_power.h"

#include <stdio.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "ST77916.h"
#include "esp_log.h"
#include "app_voice.h"

static const char *TAG = "kouki.power";
#else
// Для тестов на хосте объявляем слабую функцию Set_Backlight
__attribute__((weak)) void Set_Backlight(uint8_t Light) {
    (void)Light;
}
#endif

static uint32_t s_idle_ms = 0;
static float s_current_brightness = (float)KOUKI_POWER_BRIGHTNESS_ACTIVE;
static uint8_t s_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
static uint8_t s_applied_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;

void kouki_power_init(void)
{
    s_idle_ms = 0;
    s_current_brightness = (float)KOUKI_POWER_BRIGHTNESS_ACTIVE;
    s_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    s_applied_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    Set_Backlight(s_applied_brightness);
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Диспетчер питания инициализирован (100%% яркости)");
#endif
}

void kouki_power_notify_activity(void)
{
    s_idle_ms = 0;
    s_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
    s_current_brightness = (float)KOUKI_POWER_BRIGHTNESS_ACTIVE;
    if (s_applied_brightness != KOUKI_POWER_BRIGHTNESS_ACTIVE) {
        s_applied_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE;
        Set_Backlight(s_applied_brightness);
#ifdef ESP_PLATFORM
        ESP_LOGD(TAG, "Активность: яркость восстановлена до 100%%");
#endif
    }
}

void kouki_power_tick(uint32_t dt_ms)
{
#ifdef ESP_PLATFORM
    // Если станция не в покое (запись, отправка, озвучивание), считаем её активной
    kouki_voice_state_t vstate = KOUKI_VOICE_IDLE;
    kouki_voice_get_snapshot(&vstate, NULL);
    if (vstate != KOUKI_VOICE_IDLE) {
        kouki_power_notify_activity();
        return;
    }
#endif

    s_idle_ms += dt_ms;

    uint32_t fade_duration_ms = KOUKI_POWER_FADE_WAKE_MS;

    if (s_idle_ms >= KOUKI_POWER_IDLE_OFF_MS) {
        s_target_brightness = KOUKI_POWER_BRIGHTNESS_OFF; // 0%
        fade_duration_ms = KOUKI_POWER_FADE_OFF_MS;       // 1000 мс
    } else if (s_idle_ms >= KOUKI_POWER_IDLE_DIM_MS) {
        s_target_brightness = KOUKI_POWER_BRIGHTNESS_DIM; // 20%
        fade_duration_ms = KOUKI_POWER_FADE_DIM_MS;       // 1500 мс
    } else {
        s_target_brightness = KOUKI_POWER_BRIGHTNESS_ACTIVE; // 100%
        fade_duration_ms = KOUKI_POWER_FADE_WAKE_MS;
    }

    if ((int)s_current_brightness != s_target_brightness) {
        float step = ((float)dt_ms / (float)fade_duration_ms) * 100.0f;
        if (step < 0.5f) {
            step = 0.5f;
        }

        if (s_current_brightness > (float)s_target_brightness) {
            s_current_brightness -= step;
            if (s_current_brightness < (float)s_target_brightness) {
                s_current_brightness = (float)s_target_brightness;
            }
        } else {
            s_current_brightness += step;
            if (s_current_brightness > (float)s_target_brightness) {
                s_current_brightness = (float)s_target_brightness;
            }
        }

        uint8_t new_b = (uint8_t)(s_current_brightness + 0.5f);
        if (new_b != s_applied_brightness) {
            s_applied_brightness = new_b;
            Set_Backlight(s_applied_brightness);
        }
    }
}

uint8_t kouki_power_get_brightness(void)
{
    return s_applied_brightness;
}

uint32_t kouki_power_get_idle_ms(void)
{
    return s_idle_ms;
}
