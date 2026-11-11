/*
 * Kouki Station — голосовая станция Kouki.
 *
 * Точка входа остаётся C-файлом (спека T121 §5, решение 10.11): здесь только
 * то, что обязано произойти ДО объектного графа — инициализация NVS и ранняя
 * проверка удержания кнопки BOOT для сброса настроек. Сборка объектов
 * (kouki::station()) и прежний порядок инициализации живут в kouki_api.cpp,
 * порядок не изменился:
 *
 *   NVS -> BOOT-сброс -> kouki_config_init -> esp_board_init(16000,2,16)
 *   -> audio_out -> player -> EXIO -> LCD -> LVGL -> voice -> history -> power
 *   -> ui -> консоль -> ota -> Wi-Fi/SoftAP
 */
#include "app_config.h"
#include "app_history.h"
#include "app_power.h"
#include "app_ota.h"
#include "app_setup.h"
#include "app_ui.h"
#include "app_voice.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "kouki_api.h"
#include "lvgl.h"
#include "nvs_flash.h"

static const char *TAG = "kouki";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Ранняя проверка удержания кнопки BOOT (GPIO 0) для сброса настроек */
    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
    if (gpio_get_level(GPIO_NUM_0) == 0) {
        ESP_LOGW(TAG, "кнопка BOOT нажата при старте, удерживайте 10 с для сброса…");
        int hold_ms = 0;
        while (gpio_get_level(GPIO_NUM_0) == 0 && hold_ms < 10000) {
            vTaskDelay(pdMS_TO_TICKS(100));
            hold_ms += 100;
        }
        if (hold_ms >= 10000) {
            ESP_LOGW(TAG, "кнопка удерживалась 10 с: сброс настроек NVS!");
            kouki_config_erase();
        }
    }

    ESP_ERROR_CHECK(kouki_config_init());

    /* Объекты станции и порядок инициализации — в kouki_api.cpp. */
    ESP_ERROR_CHECK(kouki_station_wire());
    ESP_ERROR_CHECK(kouki_station_init());

    uint64_t last_tick_us = esp_timer_get_time();
    while (true) {
        /* LVGL не потокобезопасен: весь UI живёт в этой задаче. */
        lv_timer_handler();

        uint64_t now_us = esp_timer_get_time();
        uint32_t dt_ms = (uint32_t)((now_us - last_tick_us) / 1000);
        if (dt_ms >= 50) {
            kouki_power_tick(dt_ms);
            last_tick_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
