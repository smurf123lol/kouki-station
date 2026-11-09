#include "app_ota.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_speech.h"
#include "app_ui.h"
#include "app_voice.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "kouki.ota";

#define MAX_OTA_SLOT_SIZE (0x400000) /* 4 МБ */

static bool s_pending_verify = false;
static TickType_t s_verify_start_tick = 0;
static bool s_verify_timeout_logged = false;
static volatile bool s_is_updating = false;
static TaskHandle_t s_ota_task_handle = NULL;
static EventGroupHandle_t s_ota_event_group = NULL;
#define OTA_START_BIT     BIT0
#define OTA_CHECK_NOW_BIT BIT1

/* Контекст для вычисления sha256 и отслеживания загрузки */
typedef struct {
    mbedtls_sha256_context sha_ctx;
    size_t bytes_downloaded;
} ota_download_ctx_t;

int kouki_ota_semver_compare(const char *v1, const char *v2)
{
    if (v1 == NULL && v2 == NULL) return 0;
    if (v1 == NULL) return -1;
    if (v2 == NULL) return 1;

    while (*v1 == ' ' || *v1 == 'v' || *v1 == 'V') v1++;
    while (*v2 == ' ' || *v2 == 'v' || *v2 == 'V') v2++;

    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;

    const char *pre1 = NULL;
    const char *pre2 = NULL;

    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);

    pre1 = strchr(v1, '-');
    if (pre1 != NULL) pre1++;

    pre2 = strchr(v2, '-');
    if (pre2 != NULL) pre2++;

    if (maj1 != maj2) return (maj1 > maj2) ? 1 : -1;
    if (min1 != min2) return (min1 > min2) ? 1 : -1;
    if (pat1 != pat2) return (pat1 > pat2) ? 1 : -1;

    /* При равенстве major, minor, patch: версия без суффикса новее, чем с пре-релизом (1.0.0 > 1.0.0-rc) */
    if (pre1 == NULL && pre2 != NULL) return 1;
    if (pre1 != NULL && pre2 == NULL) return -1;
    if (pre1 != NULL && pre2 != NULL) return strcmp(pre1, pre2);

    return 0;
}

void kouki_ota_mark_server_communicated(void)
{
    if (s_pending_verify) {
        s_pending_verify = false;
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "связь с сервером подтверждена: образ валиден, откат отменён");
        } else {
            ESP_LOGE(TAG, "не удалось подтвердить образ: %s", esp_err_to_name(err));
        }
    }
}

bool kouki_ota_is_updating(void)
{
    return s_is_updating;
}

static esp_err_t _ota_http_client_init_cb(esp_http_client_handle_t client)
{
    const char *token = kouki_config_get_srv_token();
    if (token != NULL && token[0] != '\0') {
        char auth_hdr[300];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        return esp_http_client_set_header(client, "Authorization", auth_hdr);
    }
    return ESP_OK;
}

static esp_err_t _ota_http_event_handler(esp_http_client_event_t *evt)
{
    ota_download_ctx_t *ctx = (ota_download_ctx_t *)evt->user_data;
    if (ctx == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data_len > 0) {
            mbedtls_sha256_update(&ctx->sha_ctx, (const unsigned char *)evt->data, evt->data_len);
            ctx->bytes_downloaded += evt->data_len;
        }
    }
    return ESP_OK;
}

/* Выполняет установку новой версии прошивки */
static esp_err_t perform_ota_update(const char *image_url, const char *expected_sha256)
{
    const char *srv_url = kouki_config_get_srv_url();
    char full_url[256];
    if (image_url[0] == '/') {
        snprintf(full_url, sizeof(full_url), "%s%s", srv_url, image_url);
    } else {
        strncpy(full_url, image_url, sizeof(full_url) - 1);
        full_url[sizeof(full_url) - 1] = '\0';
    }

    /* Ожидание состояния IDLE */
    ESP_LOGI(TAG, "ожидание перехода станции в состояние покоя (IDLE)…");
    while (true) {
        kouki_voice_state_t v_state = KOUKI_VOICE_IDLE;
        kouki_voice_get_snapshot(&v_state, NULL);
        bool busy = (v_state != KOUKI_VOICE_IDLE)
                    || kouki_voice_is_recording()
                    || kouki_speech_busy()
                    || kouki_speech_is_playing();
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    s_is_updating = true;
    kouki_ui_show_ota_progress("Обновление прошивки…\nНе выключайте питание");
    ESP_LOGI(TAG, "начинаю загрузку прошивки из %s", full_url);

    ota_download_ctx_t dctx;
    memset(&dctx, 0, sizeof(dctx));
    mbedtls_sha256_init(&dctx.sha_ctx);
    mbedtls_sha256_starts(&dctx.sha_ctx, 0); /* 0 для SHA256 */

    esp_http_client_config_t http_cfg = {
        .url = full_url,
        .cert_pem = kouki_config_get_effective_ca_pem(),
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .skip_cert_common_name_check = false,
        .event_handler = _ota_http_event_handler,
        .user_data = &dctx,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
        .http_client_init_cb = _ota_http_client_init_cb,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        mbedtls_sha256_free(&dctx.sha_ctx);
        s_is_updating = false;
        kouki_ui_clear_ota();
        return err;
    }

    while (1) {
        err = esp_https_ota_perform(ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        mbedtls_sha256_free(&dctx.sha_ctx);
        s_is_updating = false;
        kouki_ui_clear_ota();
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "образ получен не полностью");
        esp_https_ota_abort(ota_handle);
        mbedtls_sha256_free(&dctx.sha_ctx);
        s_is_updating = false;
        kouki_ui_clear_ota();
        return ESP_ERR_INVALID_SIZE;
    }

    unsigned char sha_res[32];
    mbedtls_sha256_finish(&dctx.sha_ctx, sha_res);
    mbedtls_sha256_free(&dctx.sha_ctx);

    char sha_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(sha_hex + (i * 2), "%02x", sha_res[i]);
    }
    sha_hex[64] = '\0';

    if (expected_sha256 != NULL && expected_sha256[0] != '\0') {
        if (strcasecmp(sha_hex, expected_sha256) != 0) {
            ESP_LOGE(TAG, "sha256 mismatch! ожидали: %s, получили: %s", expected_sha256, sha_hex);
            esp_https_ota_abort(ota_handle);
            s_is_updating = false;
            kouki_ui_clear_ota();
            return ESP_ERR_INVALID_CRC;
        }
    }

    ESP_LOGI(TAG, "sha256 проверен: %s (размер %zu байт)", sha_hex, dctx.bytes_downloaded);

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        s_is_updating = false;
        kouki_ui_clear_ota();
        return err;
    }

    ESP_LOGI(TAG, "обновление готово! Перезагрузка…");
    kouki_ui_show_ota_progress("Обновление готово.\nПерезагрузка…");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/* Проверяет наличие обновлений на сервере */
static void check_for_updates(void)
{
    const char *srv_url = kouki_config_get_srv_url();
    const char *token = kouki_config_get_srv_token();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_ver = app_desc->version;

    char check_url[256];
    snprintf(check_url, sizeof(check_url), "%s/api/firmware?current=%s", srv_url, current_ver);

    ESP_LOGI(TAG, "проверка обновлений: %s (текущая версия %s)", check_url, current_ver);

    char auth_hdr[300] = {0};
    if (token != NULL && token[0] != '\0') {
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
    }

    esp_http_client_config_t http_cfg = {
        .url = check_url,
        .cert_pem = kouki_config_get_effective_ca_pem(),
        .timeout_ms = 10000,
        .keep_alive_enable = false,
        .skip_cert_common_name_check = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "не удалось создать http client для проверки обновлений");
        return;
    }

    if (auth_hdr[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", auth_hdr);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "не удалось подключиться к серверу для проверки OTA: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;
    int status_code = esp_http_client_get_status_code(client);

    /* Любой ответ сервера подтверждает успешное установление связи */
    if (status_code > 0) {
        kouki_ota_mark_server_communicated();
    }

    if (status_code == 401) {
        ESP_LOGW(TAG, "токен не принят (HTTP 401)");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    if (status_code != 200) {
        ESP_LOGD(TAG, "сервер вернул статус %d (обновлений нет)", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    char body[1024];
    int read_len = esp_http_client_read(client, body, sizeof(body) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGW(TAG, "пустой ответ от /api/firmware");
        return;
    }
    body[read_len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        ESP_LOGW(TAG, "не удалось разобрать JSON от /api/firmware");
        return;
    }

    cJSON *item_ver = cJSON_GetObjectItem(json, "version");
    cJSON *item_sha = cJSON_GetObjectItem(json, "sha256");
    cJSON *item_size = cJSON_GetObjectItem(json, "size");
    cJSON *item_url = cJSON_GetObjectItem(json, "url");
    cJSON *item_upd = cJSON_GetObjectItem(json, "update");

    if (item_upd != NULL && cJSON_IsFalse(item_upd)) {
        ESP_LOGI(TAG, "сервер сообщил update=false: образ не новее текущего");
        cJSON_Delete(json);
        return;
    }

    if (!cJSON_IsString(item_ver) || !cJSON_IsString(item_url)) {
        ESP_LOGW(TAG, "некорректный формат ответа /api/firmware");
        cJSON_Delete(json);
        return;
    }

    const char *new_ver = item_ver->valuestring;
    const char *img_url = item_url->valuestring;
    const char *sha256_str = (item_sha && cJSON_IsString(item_sha)) ? item_sha->valuestring : "";
    int img_size = (item_size && cJSON_IsNumber(item_size)) ? item_size->valueint : 0;

    int cmp = kouki_ota_semver_compare(new_ver, current_ver);
    if (cmp <= 0) {
        ESP_LOGI(TAG, "версия на сервере (%s) не новее текущей (%s)", new_ver, current_ver);
        cJSON_Delete(json);
        return;
    }

    if (img_size > MAX_OTA_SLOT_SIZE) {
        ESP_LOGE(TAG, "образ больше слота: %d > %d байт, отмена", img_size, MAX_OTA_SLOT_SIZE);
        cJSON_Delete(json);
        return;
    }

    ESP_LOGI(TAG, "найдена новая версия: %s (текущая: %s, размер: %d байт)", new_ver, current_ver, img_size);

    char target_url[256];
    char target_sha[65];
    strncpy(target_url, img_url, sizeof(target_url) - 1);
    target_url[sizeof(target_url) - 1] = '\0';
    strncpy(target_sha, sha256_str, sizeof(target_sha) - 1);
    target_sha[sizeof(target_sha) - 1] = '\0';
    cJSON_Delete(json);

    perform_ota_update(target_url, target_sha);
}

static void ota_task(void *arg)
{
    (void)arg;

    /* Ждем сигнала старта после подключения к сети */
    xEventGroupWaitBits(s_ota_event_group, OTA_START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    /* Небольшая пауза после подъема сети */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (true) {
        check_for_updates();

        uint32_t interval_s = kouki_config_get_ota_interval_s();
        if (interval_s == 0) {
            interval_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
        }

        /* Спим секундами, проверяя сигнал немедленной проверки и таймаут PENDING_VERIFY */
        for (uint32_t s = 0; s < interval_s; s++) {
            EventBits_t b = xEventGroupWaitBits(s_ota_event_group, OTA_CHECK_NOW_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
            if (b & OTA_CHECK_NOW_BIT) {
                ESP_LOGI(TAG, "запрошена внеочередная проверка OTA");
                break;
            }

            if (s_pending_verify && !s_verify_timeout_logged) {
                if ((xTaskGetTickCount() - s_verify_start_tick) >= pdMS_TO_TICKS(60000)) {
                    s_verify_timeout_logged = true;
                    ESP_LOGW(TAG, "60 с прошло без связи с сервером: образ остаётся в PENDING_VERIFY (при сбое будет откат)");
                }
            }
        }
    }
}

esp_err_t kouki_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                s_pending_verify = true;
                s_verify_start_tick = xTaskGetTickCount();
                ESP_LOGW(TAG, "образ запущен в PENDING_VERIFY на слоте '%s': ожидается подтверждение связи с сервером (таймаут 60 с)", running->label);
            } else {
                ESP_LOGI(TAG, "текущий активный раздел: '%s', ota_state=%d", running->label, ota_state);
            }
        }
    }

    s_ota_event_group = xEventGroupCreate();
    if (s_ota_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(ota_task, "kouki.ota", 8192, NULL, 5, &s_ota_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "не удалось создать задачу OTA");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void kouki_ota_start(void)
{
    if (s_ota_event_group != NULL) {
        xEventGroupSetBits(s_ota_event_group, OTA_START_BIT);
    }
}

void kouki_ota_check_now(void)
{
    if (s_ota_event_group != NULL) {
        xEventGroupSetBits(s_ota_event_group, OTA_CHECK_NOW_BIT);
    }
}
