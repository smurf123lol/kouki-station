#include "app_ota.hpp"

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "app_config.hpp"
#include "app_http.hpp"
#include "app_speech.h"
#include "app_ui.h"
#include "app_voice.h"
#include "app_wifi.hpp"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

namespace kouki {

namespace {

const char *const TAG = "kouki.ota";

/* Предел размера образа под единственный app-слот (4 МБ). */
constexpr size_t kMaxOtaSlotSize = 0x400000;

/* Биты группы событий: прежние OTA_START_BIT / OTA_CHECK_NOW_BIT. */
constexpr EventBits_t kStartBit = BIT0;
constexpr EventBits_t kCheckNowBit = BIT1;

/* Сколько ждём связи с сервером после старта нового образа. */
constexpr uint32_t kVerifyTimeoutMs = 60000;

/* Пауза задачи после подъёма сети и перед перезагрузкой после установки. */
constexpr uint32_t kStartPauseMs = 2000;
constexpr uint32_t kRestartPauseMs = 2000;

/* Шаг ожидания в цикле задачи: раз в секунду проверяем события и таймаут. */
constexpr uint32_t kWaitStepMs = 1000;

/* Контекст для вычисления sha256 и отслеживания загрузки: живёт на стеке
 * update(), поэтому членом класса не является. */
struct DownloadCtx {
    mbedtls_sha256_context sha_ctx;
    size_t bytes_downloaded;
};

/**
 * Колбэк esp_http_client на событие получения данных: копит sha256 и счётчик
 * байт. Экземпляра класса здесь не нужно — состояние приходит в user_data,
 * поэтому это обычная статическая функция, а не trampoline.
 */
esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    auto *ctx = static_cast<DownloadCtx *>(evt->user_data);
    if (ctx == nullptr) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data_len > 0) {
            mbedtls_sha256_update(&ctx->sha_ctx, static_cast<const unsigned char *>(evt->data),
                                  static_cast<size_t>(evt->data_len));
            ctx->bytes_downloaded += static_cast<size_t>(evt->data_len);
        }
    }
    return ESP_OK;
}

}  // namespace

Ota::Ota(Config &config, Wifi &wifi, Http &http)
    : m_config(config),
      m_wifi(wifi),
      m_http(http),
      m_pending_verify(false),
      m_verify_start_tick(0),
      m_verify_timeout_logged(false),
      m_is_updating(false),
      m_task(nullptr),
      m_events(nullptr)
{
    // m_wifi и m_http приходят по спеке §2 как зависимости модуля и держатся
    // на случай, если проверку обновлений переведут на kouki::Http. Сейчас
    // OTA-путь зовёт esp_http_client/esp_https_ota напрямую: порядок вызовов и
    // тексты логов сверяются с master построчно, а kouki::Http добавляет свою
    // обработку (pinned CA, запрет http://) — это изменило бы поведение.
}

int Ota::semver_compare(const char *v1, const char *v2)
{
    if (v1 == nullptr && v2 == nullptr) return 0;
    if (v1 == nullptr) return -1;
    if (v2 == nullptr) return 1;

    while (*v1 == ' ' || *v1 == 'v' || *v1 == 'V') v1++;
    while (*v2 == ' ' || *v2 == 'v' || *v2 == 'V') v2++;

    int maj1 = 0, min1 = 0, pat1 = 0;
    int maj2 = 0, min2 = 0, pat2 = 0;

    sscanf(v1, "%d.%d.%d", &maj1, &min1, &pat1);
    sscanf(v2, "%d.%d.%d", &maj2, &min2, &pat2);

    const char *pre1 = strchr(v1, '-');
    if (pre1 != nullptr) pre1++;

    const char *pre2 = strchr(v2, '-');
    if (pre2 != nullptr) pre2++;

    if (maj1 != maj2) return (maj1 > maj2) ? 1 : -1;
    if (min1 != min2) return (min1 > min2) ? 1 : -1;
    if (pat1 != pat2) return (pat1 > pat2) ? 1 : -1;

    /* При равенстве major, minor, patch: версия без суффикса новее, чем с пре-релизом (1.0.0 > 1.0.0-rc) */
    if (pre1 == nullptr && pre2 != nullptr) return 1;
    if (pre1 != nullptr && pre2 == nullptr) return -1;
    if (pre1 != nullptr && pre2 != nullptr) return strcmp(pre1, pre2);

    return 0;
}

void Ota::mark_server_communicated()
{
    if (m_pending_verify) {
        m_pending_verify = false;
        const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "связь с сервером подтверждена: образ валиден, откат отменён");
        } else {
            ESP_LOGE(TAG, "не удалось подтвердить образ: %s", esp_err_to_name(err));
        }
    }
}

bool Ota::is_updating() const
{
    return m_is_updating.load(std::memory_order_acquire);
}

esp_err_t Ota::on_http_client_init(esp_http_client_handle_t client)
{
    const char *token = m_config.srv_token();
    if (token != nullptr && token[0] != '\0') {
        char auth_hdr[300];
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
        return esp_http_client_set_header(client, "Authorization", auth_hdr);
    }
    return ESP_OK;
}

esp_err_t Ota::http_client_init_trampoline(esp_http_client_handle_t client)
{
    // Экземпляр один (kouki::ota()), колбэк C-шный — зовём метод через него.
    return ota().on_http_client_init(client);
}

esp_err_t Ota::update(const char *image_url, const char *expected_sha256)
{
    const char *srv_url = m_config.srv_url();
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
        const kouki_voice_state_t v_state = kouki_voice_get_state();
        const bool busy = (v_state != KOUKI_VOICE_IDLE)
                          || kouki_voice_is_recording()
                          || kouki_speech_busy()
                          || kouki_speech_is_playing()
                          || kouki_speech_worker_active();
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    m_is_updating.store(true, std::memory_order_release);
    kouki_ui_show_ota_progress("Обновление прошивки…\nНе выключайте питание");
    ESP_LOGI(TAG, "начинаю загрузку прошивки из %s", full_url);

    DownloadCtx dctx;
    memset(&dctx, 0, sizeof(dctx));
    mbedtls_sha256_init(&dctx.sha_ctx);
    mbedtls_sha256_starts(&dctx.sha_ctx, 0); /* 0 для SHA256 */

    // Поля заполняем присваиванием, а не designated initializers: в C++
    // порядок инициализаторов обязан совпадать с порядком полей структуры
    // IDF, а в C — нет; присваивание ведёт себя одинаково в обоих языках.
    esp_http_client_config_t http_cfg = {};
    http_cfg.url = full_url;
    http_cfg.cert_pem = m_config.effective_ca_pem();
    http_cfg.timeout_ms = 30000;
    http_cfg.keep_alive_enable = true;
    http_cfg.skip_cert_common_name_check = false;
    http_cfg.event_handler = on_http_event;
    http_cfg.user_data = &dctx;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;
    ota_cfg.http_client_init_cb = http_client_init_trampoline;

    esp_https_ota_handle_t ota_handle = nullptr;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        mbedtls_sha256_free(&dctx.sha_ctx);
        m_is_updating.store(false, std::memory_order_release);
        kouki_ui_clear_ota();
        return err;
    }

    while (true) {
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
        m_is_updating.store(false, std::memory_order_release);
        kouki_ui_clear_ota();
        return err;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "образ получен не полностью");
        esp_https_ota_abort(ota_handle);
        mbedtls_sha256_free(&dctx.sha_ctx);
        m_is_updating.store(false, std::memory_order_release);
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

    if (expected_sha256 != nullptr && expected_sha256[0] != '\0') {
        if (strcasecmp(sha_hex, expected_sha256) != 0) {
            ESP_LOGE(TAG, "sha256 mismatch! ожидали: %s, получили: %s", expected_sha256, sha_hex);
            esp_https_ota_abort(ota_handle);
            m_is_updating.store(false, std::memory_order_release);
            kouki_ui_clear_ota();
            return ESP_ERR_INVALID_CRC;
        }
    }

    ESP_LOGI(TAG, "sha256 проверен: %s (размер %zu байт)", sha_hex, dctx.bytes_downloaded);

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        m_is_updating.store(false, std::memory_order_release);
        kouki_ui_clear_ota();
        return err;
    }

    ESP_LOGI(TAG, "обновление готово! Перезагрузка…");
    kouki_ui_show_ota_progress("Обновление готово.\nПерезагрузка…");
    vTaskDelay(pdMS_TO_TICKS(kRestartPauseMs));
    esp_restart();
    return ESP_OK;
}

void Ota::poll_once()
{
    const char *srv_url = m_config.srv_url();
    const char *token = m_config.srv_token();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_ver = app_desc->version;

    char check_url[256];
    snprintf(check_url, sizeof(check_url), "%s/api/firmware?current=%s", srv_url, current_ver);

    ESP_LOGI(TAG, "проверка обновлений: %s (текущая версия %s)", check_url, current_ver);

    char auth_hdr[300] = {0};
    if (token != nullptr && token[0] != '\0') {
        snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", token);
    }

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = check_url;
    http_cfg.cert_pem = m_config.effective_ca_pem();
    http_cfg.timeout_ms = 10000;
    http_cfg.keep_alive_enable = false;
    http_cfg.skip_cert_common_name_check = false;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == nullptr) {
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

    const int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;
    const int status_code = esp_http_client_get_status_code(client);

    /* Любой ответ сервера подтверждает успешное установление связи */
    if (status_code > 0) {
        mark_server_communicated();
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
    const int read_len = esp_http_client_read(client, body, sizeof(body) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        ESP_LOGW(TAG, "пустой ответ от /api/firmware");
        return;
    }
    body[read_len] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (json == nullptr) {
        ESP_LOGW(TAG, "не удалось разобрать JSON от /api/firmware");
        return;
    }

    cJSON *item_ver = cJSON_GetObjectItem(json, "version");
    cJSON *item_sha = cJSON_GetObjectItem(json, "sha256");
    cJSON *item_size = cJSON_GetObjectItem(json, "size");
    cJSON *item_url = cJSON_GetObjectItem(json, "url");
    cJSON *item_upd = cJSON_GetObjectItem(json, "update");

    if (item_upd != nullptr && cJSON_IsFalse(item_upd)) {
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
    const int img_size = (item_size && cJSON_IsNumber(item_size)) ? item_size->valueint : 0;

    const int cmp = semver_compare(new_ver, current_ver);
    if (cmp <= 0) {
        ESP_LOGI(TAG, "версия на сервере (%s) не новее текущей (%s)", new_ver, current_ver);
        cJSON_Delete(json);
        return;
    }

    if (static_cast<size_t>(img_size) > kMaxOtaSlotSize) {
        ESP_LOGE(TAG, "образ больше слота: %d > %zu байт, отмена", img_size, kMaxOtaSlotSize);
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

    update(target_url, target_sha);
}

void Ota::poll_verify_timeout()
{
    if (m_pending_verify && !m_verify_timeout_logged) {
        if ((xTaskGetTickCount() - m_verify_start_tick) >= pdMS_TO_TICKS(kVerifyTimeoutMs)) {
            m_verify_timeout_logged = true;
            ESP_LOGW(TAG, "60 с прошло без связи с сервером: образ остаётся в PENDING_VERIFY (при сбое будет откат)");
        }
    }
}

void Ota::ota_task_trampoline(void *arg)
{
    // Указатель на экземпляр приходит из xTaskCreate, как в прежнем C-модуле
    // приходил NULL: состояние теперь не файловое.
    if (arg != nullptr) {
        static_cast<Ota *>(arg)->run_task();
    }
    vTaskDelete(nullptr);
}

void Ota::run_task()
{
    /* Ждем сигнала старта после подключения к сети */
    xEventGroupWaitBits(m_events, kStartBit, pdFALSE, pdTRUE, portMAX_DELAY);

    /* Небольшая пауза после подъема сети */
    vTaskDelay(pdMS_TO_TICKS(kStartPauseMs));

    while (true) {
        poll_once();

        uint32_t interval_s = m_config.ota_interval_s();
        if (interval_s == 0) {
            interval_s = KOUKI_CONFIG_DEFAULT_OTA_INTERVAL;
        }

        /* Спим секундами, проверяя сигнал немедленной проверки и таймаут PENDING_VERIFY */
        for (uint32_t s = 0; s < interval_s; s++) {
            const EventBits_t b = xEventGroupWaitBits(m_events, kCheckNowBit, pdTRUE, pdFALSE,
                                                      pdMS_TO_TICKS(kWaitStepMs));
            if (b & kCheckNowBit) {
                ESP_LOGI(TAG, "запрошена внеочередная проверка OTA");
                break;
            }

            poll_verify_timeout();
        }
    }
}

esp_err_t Ota::init()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != nullptr) {
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                m_pending_verify = true;
                m_verify_start_tick = xTaskGetTickCount();
                ESP_LOGW(TAG, "образ запущен в PENDING_VERIFY на слоте '%s': ожидается подтверждение связи с сервером (таймаут 60 с)", running->label);
            } else {
                ESP_LOGI(TAG, "текущий активный раздел: '%s', ota_state=%d", running->label, ota_state);
            }
        }
    }

    m_events = xEventGroupCreate();
    if (m_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const BaseType_t ret = xTaskCreate(ota_task_trampoline, "kouki.ota", 8192, this, 5, &m_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "не удалось создать задачу OTA");
        return ESP_FAIL;
    }

    return ESP_OK;
}

void Ota::start()
{
    if (m_events != nullptr) {
        xEventGroupSetBits(m_events, kStartBit);
    }
}

void Ota::check_now()
{
    if (m_events != nullptr) {
        xEventGroupSetBits(m_events, kCheckNowBit);
    }
}

Ota &ota()
{
    static Ota s_instance(config(), wifi(), http());
    return s_instance;
}

}  // namespace kouki

// Публичный C-API: тонкий фасад на единственный экземпляр.
extern "C" {

esp_err_t kouki_ota_init(void)
{
    return kouki::ota().init();
}

void kouki_ota_start(void)
{
    kouki::ota().start();
}

void kouki_ota_check_now(void)
{
    kouki::ota().check_now();
}

void kouki_ota_mark_server_communicated(void)
{
    kouki::ota().mark_server_communicated();
}

bool kouki_ota_is_updating(void)
{
    return kouki::ota().is_updating();
}

int kouki_ota_semver_compare(const char *v1, const char *v2)
{
    return kouki::Ota::semver_compare(v1, v2);
}

}  // extern "C"
