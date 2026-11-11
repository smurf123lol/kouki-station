#include "kouki_api.hpp"

#include <cstdio>

#include "app_audio_out.hpp"
#include "app_ui.h"
#include "app_config.hpp"
#include "app_history.hpp"
#include "app_http.hpp"
#include "app_ota.hpp"
#include "app_power.hpp"
#include "app_setup.hpp"
#include "app_speech.hpp"
#include "app_time.hpp"
#include "app_tone.hpp"
#include "app_vad.hpp"
#include "app_voice.hpp"
#include "app_wake.hpp"
#include "app_wake_verify.hpp"
#include "app_wifi.hpp"

#include "driver/gpio.h"
#include "esp_board_init.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Вендорные драйверы Waveshare — C-файлы без extern "C" в своих заголовках.
 * Подключать их .h из C++ нельзя: имена замэнглятся, и линковка упадёт на
 * _Z9EXIO_Initv (проверено). Поэтому объявляем ровно те три функции, что нужны
 * порядку инициализации, в extern "C"; сами драйверы не правим (AGENTS.md п.1).
 */
extern "C" {
esp_err_t EXIO_Init(void);
void LCD_Init(void);
void LVGL_Init(void);
}

namespace kouki {

namespace {

/* Прежний TAG точки входа: тексты логов станции остаются дословными. */
const char *const kTag = "kouki";

/*
 * Усилитель динамика и громкость кодека (перенесено из main.c без изменений).
 *
 * Две вещи, без которых кодек отвечает ESP_OK, а звука нет:
 *  - NS4150B включается высоким уровнем на GPIO 15 (в вендорном audio_board
 *    GPIO_PWR_CTRL = -1, то есть нога не поднята);
 *  - громкость в bsp_board.c ставится до esp_codec_dev_open, а ES8311 такой
 *    вызов игнорирует — ставим её ещё раз, уже после открытия.
 */
void player_init()
{
#if CONFIG_KOUKI_PLAYER_PA_GPIO >= 0
    const gpio_num_t pa = (gpio_num_t)CONFIG_KOUKI_PLAYER_PA_GPIO;
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_KOUKI_PLAYER_PA_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t pa_err = gpio_config(&cfg);
    if (pa_err == ESP_OK) {
        gpio_set_level(pa, 1);
        ESP_LOGI(kTag, "усилитель динамика включён (GPIO %d)", CONFIG_KOUKI_PLAYER_PA_GPIO);
    } else {
        ESP_LOGW(kTag, "не удалось поднять усилитель (GPIO %d): %s",
                 CONFIG_KOUKI_PLAYER_PA_GPIO, esp_err_to_name(pa_err));
    }
#endif

    audio_out().lock();
    const esp_err_t vol_err = esp_audio_set_play_vol(CONFIG_KOUKI_PLAYER_VOLUME);
    audio_out().unlock();
    if (vol_err == ESP_OK) {
        ESP_LOGI(kTag, "громкость выхода: %d %%", CONFIG_KOUKI_PLAYER_VOLUME);
    } else {
        ESP_LOGW(kTag, "кодек не принял громкость %d: %s", CONFIG_KOUKI_PLAYER_VOLUME,
                 esp_err_to_name(vol_err));
    }
}

/*
 * Сеть поднимается отдельно: экран должен ожить даже без Wi-Fi.
 *
 * Задача и её тело перенесены из main.c как хвост порядка инициализации
 * (Wi-Fi/SoftAP — последний шаг списка спеки §1); логика 1:1.
 */
void wifi_task(void *arg)
{
    (void)arg;

    if (wifi().connect() == ESP_OK) {
        ESP_LOGI(kTag, "сеть готова, сервер %s", config().srv_url());
        ota().start();
#if CONFIG_KOUKI_DEBUG_MIC
        /* Диагностика входа: раз в 6 секунд печатаем уровни, чтобы можно было
         * поговорить в этот момент и увидеть, какой канал живой. */
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(6000));
            voice_engine().debug_levels();
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
        voice_engine().set_session(session);
        ESP_LOGW(kTag, "тестовый режим: пауза %d с, запись %d с, отправлю как %s",
                 CONFIG_KOUKI_TEST_MODE_DELAY_S, CONFIG_KOUKI_TEST_MODE_SECONDS,
                 session == KOUKI_SESSION_ASK ? "вопрос" : "задачу");
        vTaskDelay(pdMS_TO_TICKS(CONFIG_KOUKI_TEST_MODE_DELAY_S * 1000));
        if (voice_engine().start_recording() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_KOUKI_TEST_MODE_SECONDS * 1000));
            voice_engine().stop_and_send();
        } else {
            ESP_LOGE(kTag, "тестовый режим: запись не началась");
        }
#endif
    } else {
        ESP_LOGE(kTag, "без сети: кнопка будет отвечать ошибкой отправки");
    }

    vTaskDelete(NULL);
}

}  // namespace

Station &station()
{
    /* Порядок инициализации ссылок: зависимость раньше зависимого
     * (Config -> Http/Wifi -> Ota; AudioOut -> Tone -> Speech; Vad/Wake ->
     * Voice). Сами конструкторы побочных эффектов не имеют. */
    static Station s_station{
        config(),
        timekeeper(),
        history(),
        power(),
        http(),
        audio_out(),
        tone(),
        vad(),
        wifi(),
        ota(),
        wake(),
        wake_verify(),
        speech(),
        setup(),
        voice_engine(),
    };
    return s_station;
}

esp_err_t station_wire()
{
    /* Обращение к station() создаёт все объекты сразу (function-local static:
     * граф живёт всё время работы прошивки, динамики нет). Новых логов не
     * добавляем: вывод станции должен остаться дословно прежним. */
    (void)station();
    return ESP_OK;
}

esp_err_t station_init()
{
    /*
     * Порядок строго как в заводском примере: сначала аудио-борда — она создаёт
     * I2C-шину (i2c_new_master_bus), без которой TCA9554PWR не может открыть
     * устройство; потом расширитель GPIO (через него сбрасываются LCD и тач),
     * затем LCD и LVGL.
     */
    esp_err_t err = esp_board_init(16000, 2, 16);
    if (err != ESP_OK) {
        return err;
    }

    err = audio_out().init();
    if (err != ESP_OK) {
        return err;
    }
    player_init();

    EXIO_Init();
    LCD_Init();
    LVGL_Init();

    err = voice_engine().init(nullptr, nullptr);
    if (err != ESP_OK) {
        return err;
    }
    history().init();
    power().init();
    kouki_ui_init();
    setup().console_init();
    err = ota().init();
    if (err != ESP_OK) {
        return err;
    }

    if (!config().is_complete()) {
        ESP_LOGW(kTag, "конфигурация не настроена: переход в режим SoftAP настройки");
        setup().start();
    } else {
        ESP_LOGI(kTag, "конфигурация готова, подключение к сети");
        if (xTaskCreate(wifi_task, "kouki.wifi", 6144, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(kTag, "не удалось создать задачу Wi-Fi");
        }
    }

    ESP_LOGI(kTag, "станция готова: %d с записи на одну фразу", CONFIG_KOUKI_MAX_RECORD_S);
    return ESP_OK;
}

}  // namespace kouki

/* C-точка входа для main.c: он остаётся C-файлом (спека §5), поэтому наружу
 * торчат только две функции — сборка графа и прежний порядок инициализации. */
extern "C" esp_err_t kouki_station_wire(void)
{
    return kouki::station_wire();
}

extern "C" esp_err_t kouki_station_init(void)
{
    return kouki::station_init();
}
