/*
 * Мок IDF-поверхности для host-стенда Ota (T121 П8), см. ota_mock.h.
 *
 * Компилируется gcc как C (как и остальные моки стендов). Закрывает разделы и
 * состояние образа (esp_ota_ops), HTTPS-OTA (esp_https_ota), задачу с
 * «часами» (freertos) и перезагрузку (esp_system). HTTP-клиент — общий
 * support/http_mock/, esp_tls_errors.h — его же.
 */
#include "ota_mock.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

/* --------------------------------------------------------------- состояние -- */

#define OTA_MOCK_MAX_IMAGE 8192

/* Активный раздел: адрес и размер как у настоящего app-слота (4 МБ). */
static esp_partition_t s_running_partition = {
    .address = 0x10000,
    .size = 0x400000,
    .label = "ota_0",
};

static struct {
    esp_ota_img_states_t image_state;
    const char *running_label;
    int mark_valid_calls;
    esp_err_t fail_mark_valid;

    esp_https_ota_config_t last_ota_cfg;
    bool have_ota_cfg;
    /* Конфигурацию клиента копируем: в образе она живёт на стеке update() и
     * умирает вместе с кадром, а стенд проверяет URL/таймаут уже после
     * возврата из update(). Копия нужна только для осмотра из теста. */
    esp_http_client_config_t http_cfg_copy;
    char url_copy[256];
    int init_cb_calls;
    esp_err_t init_cb_result;

    int perform_in_progress_rounds;
    int perform_calls;
    esp_err_t perform_final_code;
    esp_err_t fail_begin;
    esp_err_t fail_finish;
    bool complete;
    int begin_calls;
    int finish_calls;
    int abort_calls;

    unsigned char image[OTA_MOCK_MAX_IMAGE];
    size_t image_len;
    bool image_delivered;

    int task_create_calls;
    BaseType_t fail_task_create;
    int task_stack;
    int task_priority;
    void (*task_fn)(void *);
    void *task_arg;

    uint32_t ticks;
    int delay_calls;
    uint32_t delay_total_ms;

    int restart_calls;
} s_mock;

/* Одна «задача» на стенд — как и в образе. */
static struct ota_task_mock {
    int unused;
} s_task;

/* ------------------------------------------------------------- esp_ota_ops -- */

const esp_partition_t *esp_ota_get_running_partition(void)
{
    s_running_partition.label = s_mock.running_label;
    return &s_running_partition;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state)
{
    (void)partition;
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *state = s_mock.image_state;
    return ESP_OK;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    s_mock.mark_valid_calls++;
    if (s_mock.fail_mark_valid != ESP_OK) {
        return s_mock.fail_mark_valid;
    }
    s_mock.image_state = ESP_OTA_IMG_VALID;
    return ESP_OK;
}

/* ------------------------------------------------------------- esp_system -- */

void esp_restart(void)
{
    s_mock.restart_calls++;
}

/* ---------------------------------------------------------- esp_https_ota -- */

/* Доставить сохранённый колбэк ON_DATA с байтами образа (как IDF при чтении). */
static void deliver_image(void)
{
    if (s_mock.image_delivered || s_mock.image_len == 0 || !s_mock.have_ota_cfg) {
        return;
    }
    const esp_http_client_config_t *http_cfg = s_mock.last_ota_cfg.http_config;
    if (http_cfg == NULL || http_cfg->event_handler == NULL) {
        return;
    }

    esp_http_client_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.event_id = HTTP_EVENT_ON_DATA;
    evt.data = s_mock.image;
    evt.data_len = (int)s_mock.image_len;
    evt.user_data = http_cfg->user_data;
    (void)http_cfg->event_handler(&evt);
    s_mock.image_delivered = true;
}

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *ota_config, esp_https_ota_handle_t *handle)
{
    s_mock.begin_calls++;
    if (s_mock.fail_begin != ESP_OK) {
        *handle = NULL;
        return s_mock.fail_begin;
    }
    if (ota_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_mock.last_ota_cfg = *ota_config;
    s_mock.have_ota_cfg = true;

    if (ota_config->http_config != NULL) {
        s_mock.http_cfg_copy = *ota_config->http_config;
        if (ota_config->http_config->url != NULL) {
            snprintf(s_mock.url_copy, sizeof(s_mock.url_copy), "%s", ota_config->http_config->url);
            s_mock.http_cfg_copy.url = s_mock.url_copy;
        }
        s_mock.last_ota_cfg.http_config = &s_mock.http_cfg_copy;
    }

    /* IDF зовёт http_client_init_cb после создания клиента — стенд так же.
     * Сам клиент моку не нужен: колбэк ставит заголовок через общий http_mock,
     * который handle игнорирует. */
    if (ota_config->http_client_init_cb != NULL) {
        s_mock.init_cb_calls++;
        static int s_dummy_client;
        s_mock.init_cb_result = ota_config->http_client_init_cb((esp_http_client_handle_t)&s_dummy_client);
    }

    *handle = (esp_https_ota_handle_t)&s_mock;
    return ESP_OK;
}

esp_err_t esp_https_ota_perform(esp_https_ota_handle_t handle)
{
    (void)handle;
    s_mock.perform_calls++;
    deliver_image();
    if (s_mock.perform_calls <= s_mock.perform_in_progress_rounds) {
        return ESP_ERR_HTTPS_OTA_IN_PROGRESS;
    }
    return s_mock.perform_final_code;
}

bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t handle)
{
    (void)handle;
    return s_mock.complete;
}

esp_err_t esp_https_ota_finish(esp_https_ota_handle_t handle)
{
    (void)handle;
    s_mock.finish_calls++;
    return s_mock.fail_finish;
}

esp_err_t esp_https_ota_abort(esp_https_ota_handle_t handle)
{
    (void)handle;
    s_mock.abort_calls++;
    return ESP_OK;
}

/* ---------------------------------------------------------------- freertos -- */

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *out)
{
    (void)name;
    s_mock.task_create_calls++;
    s_mock.task_stack = (int)stack;
    if (s_mock.fail_task_create != pdPASS) {
        const BaseType_t ret = s_mock.fail_task_create;
        s_mock.fail_task_create = pdPASS;
        return ret;
    }
    s_mock.task_priority = (int)priority;
    s_mock.task_fn = task;
    s_mock.task_arg = arg;
    if (out != NULL) {
        *out = &s_task;
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    s_mock.delay_calls++;
    s_mock.delay_total_ms += (uint32_t)ticks;
    s_mock.ticks += (uint32_t)ticks;
}

TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)s_mock.ticks;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

/* --------------------------------------------------------------- управление -- */

void ota_mock_reset(void)
{
    memset(&s_mock, 0, sizeof(s_mock));
    s_mock.image_state = ESP_OTA_IMG_VALID;
    s_mock.running_label = "ota_0";
    s_mock.complete = true;
    s_mock.perform_final_code = ESP_OK;
    s_mock.fail_begin = ESP_OK;
    s_mock.fail_finish = ESP_OK;
    s_mock.fail_mark_valid = ESP_OK;
    /* memset обнулил и поле отказа xTaskCreate: 0 — это не pdPASS, поэтому
     * без этой строки «ошибка создания задачи» включалась бы всегда. */
    s_mock.fail_task_create = pdPASS;
}

void ota_mock_set_image_state(int state)
{
    s_mock.image_state = (esp_ota_img_states_t)state;
}

void ota_mock_set_running_label(const char *label)
{
    s_mock.running_label = label;
}

int ota_mock_mark_valid_calls(void)
{
    return s_mock.mark_valid_calls;
}

void ota_mock_fail_mark_valid(int err)
{
    s_mock.fail_mark_valid = (esp_err_t)err;
}

const char *ota_mock_last_url(void)
{
    if (!s_mock.have_ota_cfg || s_mock.last_ota_cfg.http_config == NULL) {
        return NULL;
    }
    return s_mock.last_ota_cfg.http_config->url;
}

const char *ota_mock_auth_header(void)
{
    if (!s_mock.have_ota_cfg || s_mock.last_ota_cfg.http_config == NULL) {
        return NULL;
    }
    /* Колбэк инициализации получает клиент, а не конфигурацию; заголовок он
     * ставит через esp_http_client_set_header — его и смотрим. */
    return http_mock_header("Authorization");
}

int ota_mock_init_cb_calls(void)
{
    return s_mock.init_cb_calls;
}

int ota_mock_init_cb_result(void)
{
    return s_mock.init_cb_result;
}

void ota_mock_set_perform(int in_progress_rounds, int final_code)
{
    s_mock.perform_in_progress_rounds = in_progress_rounds;
    s_mock.perform_final_code = (esp_err_t)final_code;
}

void ota_mock_fail_begin(int err)
{
    s_mock.fail_begin = (esp_err_t)err;
}

void ota_mock_fail_finish(int err)
{
    s_mock.fail_finish = (esp_err_t)err;
}

void ota_mock_set_complete(bool complete)
{
    s_mock.complete = complete;
}

void ota_mock_set_image(const void *data, size_t len)
{
    if (data == NULL || len > OTA_MOCK_MAX_IMAGE) {
        s_mock.image_len = 0;
        return;
    }
    memcpy(s_mock.image, data, len);
    s_mock.image_len = len;
    s_mock.image_delivered = false;
}

int ota_mock_begin_calls(void)
{
    return s_mock.begin_calls;
}

int ota_mock_perform_calls(void)
{
    return s_mock.perform_calls;
}

int ota_mock_finish_calls(void)
{
    return s_mock.finish_calls;
}

int ota_mock_abort_calls(void)
{
    return s_mock.abort_calls;
}

int ota_mock_http_timeout_ms(void)
{
    if (!s_mock.have_ota_cfg || s_mock.last_ota_cfg.http_config == NULL) {
        return -1;
    }
    return s_mock.last_ota_cfg.http_config->timeout_ms;
}

bool ota_mock_keep_alive(void)
{
    if (!s_mock.have_ota_cfg || s_mock.last_ota_cfg.http_config == NULL) {
        return false;
    }
    return s_mock.last_ota_cfg.http_config->keep_alive_enable;
}

void ota_mock_fail_task_create(int ret)
{
    s_mock.fail_task_create = (BaseType_t)ret;
}

int ota_mock_task_create_calls(void)
{
    return s_mock.task_create_calls;
}

int ota_mock_task_stack(void)
{
    return s_mock.task_stack;
}

int ota_mock_task_priority(void)
{
    return s_mock.task_priority;
}

void ota_mock_advance_ticks(uint32_t ms)
{
    s_mock.ticks += ms;
}

uint32_t ota_mock_ticks(void)
{
    return s_mock.ticks;
}

int ota_mock_delay_calls(void)
{
    return s_mock.delay_calls;
}

uint32_t ota_mock_delay_total_ms(void)
{
    return s_mock.delay_total_ms;
}

int ota_mock_restart_calls(void)
{
    return s_mock.restart_calls;
}

