/*
 * Kouki Station — голосовая станция Kouki.
 *
 * Порядок инициализации повторяет заводской пример Waveshare:
 * аудио-борда (создаёт I2C и кодек) → EXIO (расширитель GPIO) → LCD (QSPI,
 * ST77916) → LVGL. Аудио раньше экрана, потому что расширитель GPIO и тач
 * живут на I2C, который поднимает именно аудио-борда.
 */
#include "app_ui.h"
#include "app_voice.h"
#include "app_wifi.h"
#include "driver/gpio.h"
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

/*
 * Усилитель NS4150B включается высоким уровнем на GPIO 15 — это видно по
 * заводскому Arduino-примеру (pinMode(15, OUTPUT); digitalWrite(15, HIGH)).
 * В нашем ESP-IDF-компоненте audio_board эта нога отключена (-1), поэтому
 * кодек открывается и принимает данные, а динамик молчит.
 */
static void speaker_amp_enable(void)
{
#if CONFIG_KOUKI_PLAYER_PA_GPIO >= 0
    const gpio_num_t pin = (gpio_num_t)CONFIG_KOUKI_PLAYER_PA_GPIO;
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&config);
    gpio_set_level(pin, 1);
    ESP_LOGI(TAG, "усилитель динамика (пин %d): %s", pin, esp_err_to_name(err));
#endif
}


/* Сеть поднимается отдельно: экран должен ожить даже без Wi-Fi. */
static void wifi_task(void *arg)
{
    (void)arg;

    if (kouki_wifi_connect() == ESP_OK) {
        ESP_LOGI(TAG, "сеть готова, сервер %s", CONFIG_KOUKI_SERVER_URL);
#if CONFIG_KOUKI_DEBUG_MIC
    /* Диагностика входа: раз в 6 секунд печатаем уровни, чтобы можно было
     * поговорить в этот момент и увидеть, какой канал живой. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(6000));
        kouki_voice_debug_levels();
    }
#endif

#if CONFIG_KOUKI_TEST_MODE
        /* Тестовый режим: проверка тракта без человека. Станция выжидает,
         * пишет несколько секунд и отправляет фразу так же, как это сделала
         * бы кнопка — тем же путём, с той же сессией. */
        kouki_session_t session =
#if CONFIG_KOUKI_TEST_MODE_AS_QUESTION
            KOUKI_SESSION_ASK;
#else
            KOUKI_SESSION_TASK;
#endif
        kouki_voice_set_session(session);
        ESP_LOGW(TAG, "тестовый режим: пауза %d с, запись %d с, отправлю как %s",
                 CONFIG_KOUKI_TEST_MODE_DELAY_S, CONFIG_KOUKI_TEST_MODE_SECONDS,
                 session == KOUKI_SESSION_ASK ? "вопрос" : "задачу");
        vTaskDelay(pdMS_TO_TICKS(CONFIG_KOUKI_TEST_MODE_DELAY_S * 1000));
        if (kouki_voice_start_recording() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_KOUKI_TEST_MODE_SECONDS * 1000));
            kouki_voice_stop_and_send();
        } else {
            ESP_LOGE(TAG, "тестовый режим: запись не началась");
        }
#endif
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
     * Порядок строго как в заводском примере: сначала аудио-борда — она создаёт
     * I2C-шину (i2c_new_master_bus), без которой TCA9554PWR не может открыть
     * устройство; потом расширитель GPIO (через него сбрасываются LCD и тач),
     * затем LCD и LVGL.
     */
    ESP_ERROR_CHECK(esp_board_init(16000, 2, 16));
    speaker_amp_enable();

    /*
     * Заводской bsp выставляет громкость ДО открытия кодека, а ES8311 такой
     * регистр игнорирует — динамик остаётся немым. Повторяем после открытия.
     */
    esp_err_t volume = esp_audio_set_play_vol(CONFIG_KOUKI_PLAYER_VOLUME);
    ESP_LOGI(TAG, "громкость динамика %d: %s", CONFIG_KOUKI_PLAYER_VOLUME, esp_err_to_name(volume));

    EXIO_Init();
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
