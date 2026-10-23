/*
 * Kouki Station — голосовая станция Kouki.
 *
 * Порядок инициализации повторяет заводской пример Waveshare:
 * EXIO (расширитель GPIO) → LCD (QSPI, ST77916) → LVGL → аудио-борда.
 * Аудио инициализируется после экрана, чтобы кодек ES8311/ES7210 не мешал
 * поднятию QSPI-шины: у платы они делят I2C, но разные тайминги.
 */
#include "app_ui.h"
#include "app_voice.h"
#include "app_wifi.h"
#include "esp_board_init.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "LVGL_Driver.h"
#include "ST77916.h"
#include "TCA9554PWR.h"

static const char *TAG = "kouki";

/* Сеть поднимается отдельно: экран должен ожить даже без Wi-Fi. */
static void wifi_task(void *arg)
{
    (void)arg;

    if (kouki_wifi_connect() == ESP_OK) {
        ESP_LOGI(TAG, "сеть готова, сервер %s", CONFIG_KOUKI_SERVER_URL);
    } else {
        ESP_LOGE(TAG, "без сети: кнопка будет отвечать ошибкой отправки");
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /*
     * Порядок как в заводском примере: расширитель GPIO → аудио-борда (он же
     * поднимает I2C и кодек) → LCD → LVGL. Аудио раньше экрана, потому что
     * кодек ES8311 сидит на том же I2C, что и ST77916, и должен быть готов
     * до первого обращения к дисплею.
     */
    EXIO_Init();
    ESP_ERROR_CHECK(esp_board_init(16000, 2, 16));
    LCD_Init();
    LVGL_Init();

    ESP_ERROR_CHECK(kouki_voice_init(NULL, NULL));
    kouki_ui_init();

    if (xTaskCreate(wifi_task, "kouki.wifi", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "не удалось создать задачу Wi-Fi");
    }

    ESP_LOGI(TAG, "станция готова: %d с записи на одну фразу", CONFIG_KOUKI_MAX_RECORD_S);

    while (true) {
        /* LVGL не потокобезопасен: весь UI живёт в этой задаче. */
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
