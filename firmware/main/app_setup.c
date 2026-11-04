#include "app_setup.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_ui.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kouki.setup";

static bool s_setup_active = false;
static httpd_handle_t s_server = NULL;

static void url_decode(char *dst, const char *src, size_t dst_max)
{
    size_t d = 0;
    while (*src && d + 1 < dst_max) {
        if (*src == '%' && isxdigit((unsigned char)*(src + 1)) && isxdigit((unsigned char)*(src + 2))) {
            char hex[3] = { *(src + 1), *(src + 2), '\0' };
            dst[d++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[d++] = ' ';
            src++;
        } else {
            dst[d++] = *src++;
        }
    }
    dst[d] = '\0';
}

static bool get_form_param(const char *body, const char *key, char *out, size_t out_max)
{
    const size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val_start = p + key_len + 1;
            const char *val_end = strchr(val_start, '&');
            size_t val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
            char enc[300];
            if (val_len >= sizeof(enc)) {
                val_len = sizeof(enc) - 1;
            }
            memcpy(enc, val_start, val_len);
            enc[val_len] = '\0';
            url_decode(out, enc, out_max);
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    out[0] = '\0';
    return false;
}

static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Kouki Station — Настройка</title><style>"
        "* { box-sizing: border-box; }"
        "body { background: #0b0a08; color: #e2d2b2; font-family: sans-serif; margin: 0; padding: 20px; display: flex; justify-content: center; }"
        ".card { width: 100%; max-width: 400px; background: #16130f; border: 1px solid #332d24; border-radius: 12px; padding: 20px; }"
        "h1 { font-size: 20px; letter-spacing: 2px; text-align: center; margin: 0 0 6px; color: #e2d2b2; }"
        "p.sub { font-size: 13px; color: #8b7d66; text-align: center; margin: 0 0 20px; }"
        ".field { margin-bottom: 14px; }"
        "label { display: block; font-size: 13px; color: #8b7d66; margin-bottom: 5px; }"
        "input { width: 100%; padding: 10px; background: #0b0a08; border: 1px solid #332d24; border-radius: 6px; color: #e2d2b2; font-size: 14px; outline: none; }"
        "input:focus { border-color: #d8552f; }"
        ".hint { font-size: 11px; color: #635745; margin-top: 4px; }"
        "button { width: 100%; padding: 12px; margin-top: 10px; background: #d8552f; color: #0b0a08; font-weight: bold; border: none; border-radius: 6px; font-size: 15px; cursor: pointer; }"
        "</style></head><body><div class=\"card\">"
        "<h1>K O U K I</h1><p class=\"sub\">Первичная настройка станции</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<div class=\"field\"><label>Имя сети Wi-Fi (SSID)</label><input type=\"text\" name=\"wifi_ssid\" maxlength=\"32\" required placeholder=\"MyHomeWiFi\"></div>"
        "<div class=\"field\"><label>Пароль Wi-Fi</label><input type=\"password\" name=\"wifi_pass\" maxlength=\"64\" placeholder=\"оставьте пустым для открытой\"></div>"
        "<div class=\"field\"><label>Адрес сервера</label><input type=\"text\" name=\"srv_url\" maxlength=\"128\" required placeholder=\"http://192.168.1.157:5300\"><div class=\"hint\">http:// или https://</div></div>"
        "<div class=\"field\"><label>Bearer-токен</label><input type=\"password\" name=\"srv_token\" minlength=\"8\" maxlength=\"256\" required placeholder=\"мин. 8 символов\"></div>"
        "<div class=\"field\"><label>Метка источника</label><input type=\"text\" name=\"dev_source\" maxlength=\"32\" value=\"station\"></div>"
        "<button type=\"submit\">Сохранить</button>"
        "</form></div></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[1024];
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Запрос слишком велик");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, remaining);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Ошибка чтения");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    char ssid[64] = {0};
    char pass[128] = {0};
    char url[256] = {0};
    char token[300] = {0};
    char src[64] = {0};

    get_form_param(buf, "wifi_ssid", ssid, sizeof(ssid));
    get_form_param(buf, "wifi_pass", pass, sizeof(pass));
    get_form_param(buf, "srv_url", url, sizeof(url));
    get_form_param(buf, "srv_token", token, sizeof(token));
    get_form_param(buf, "dev_source", src, sizeof(src));

    if (strlen(ssid) == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, "SSID не может быть пустым", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, "URL должен начинаться с http:// или https://", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (strlen(token) < KOUKI_CONFIG_MIN_TOKEN_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_send(req, "Токен слишком короткий", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    kouki_config_set_wifi(ssid, pass);
    kouki_config_set_url(url);
    kouki_config_set_token(token);
    kouki_config_set_source(src[0] ? src : KOUKI_CONFIG_DEFAULT_SOURCE);

    esp_err_t err = kouki_config_save();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Ошибка записи в NVS");
        return ESP_FAIL;
    }

    const char *resp =
        "<!DOCTYPE html><html lang=\"ru\"><head><meta charset=\"utf-8\"><title>Kouki</title>"
        "<style>body{background:#0b0a08;color:#e2d2b2;font-family:sans-serif;text-align:center;padding:50px 20px;}"
        "h2{color:#38d483;}</style></head><body>"
        "<h2>Сохранено. Перезагрузка…</h2>"
        "<p>Станция перезагружается и подключается к вашей сети.</p></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    xTaskCreate(delayed_restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static const httpd_uri_t uri_get_index = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_get_index_html = {
    .uri = "/index.html",
    .method = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_post_save = {
    .uri = "/save",
    .method = HTTP_POST,
    .handler = save_post_handler,
    .user_ctx = NULL,
};

esp_err_t kouki_setup_start(void)
{
    s_setup_active = true;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Kouki-Setup-%02X%02X", mac[4], mac[5]);

    /* Оповещаем UI */
    kouki_ui_show_setup(ap_ssid, "http://192.168.4.1");

    /* Инициализируем сеть в режиме SoftAP */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_ap();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.ap.ssid, ap_ssid, sizeof(wifi_cfg.ap.ssid) - 1);
    wifi_cfg.ap.channel = 1;
    wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.ap.max_connection = 4;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "esp_wifi_set_mode(AP)");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "esp_wifi_set_config(AP)");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    ESP_LOGI(TAG, "SoftAP запущен: %s (IP 192.168.4.1)", ap_ssid);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    err = httpd_start(&s_server, &config);
    if (err == ESP_OK) {
        httpd_register_uri_handler(s_server, &uri_get_index);
        httpd_register_uri_handler(s_server, &uri_get_index_html);
        httpd_register_uri_handler(s_server, &uri_post_save);
        ESP_LOGI(TAG, "веб-сервер настройки поднят на порту 80");
    } else {
        ESP_LOGE(TAG, "не удалось запустить веб-сервер: %s", esp_err_to_name(err));
    }

    return err;
}

bool kouki_setup_is_active(void)
{
    return s_setup_active;
}

static void handle_console_command(const char *cmd)
{
    if (strcmp(cmd, "show") == 0) {
        char buf[512];
        kouki_config_format_masked(buf, sizeof(buf));
        printf("\n=== Текущие настройки ===\n%s\n", buf);
    } else if (strcmp(cmd, "help") == 0) {
        printf("\nКоманды консоли:\n"
               "  set_wifi <ssid> [pass]  — задать Wi-Fi (SSID и опциональный пароль)\n"
               "  set_token <token>       — задать bearer-токен сервера (мин. 8 символов)\n"
               "  set_url <url>           — задать URL сервера (http:// или https://)\n"
               "  set_source <source>     — задать метку источника (по умолчанию 'station')\n"
               "  show                    — показать текущие настройки (секреты скрыты ***)\n"
               "  save                    — сохранить настройки в NVS\n"
               "  reset                   — очистить настройки в NVS\n"
               "  reboot                  — перезагрузить станцию\n"
               "  help                    — эта справка\n\n");
    } else if (strncmp(cmd, "set_wifi ", 9) == 0) {
        const char *args = cmd + 9;
        while (*args == ' ') args++;
        const char *space = strchr(args, ' ');
        char ssid[40] = {0};
        char pass[80] = {0};
        if (space != NULL) {
            size_t slen = (size_t)(space - args);
            if (slen >= sizeof(ssid)) slen = sizeof(ssid) - 1;
            memcpy(ssid, args, slen);
            const char *p = space + 1;
            while (*p == ' ') p++;
            strncpy(pass, p, sizeof(pass) - 1);
        } else {
            strncpy(ssid, args, sizeof(ssid) - 1);
        }
        esp_err_t err = kouki_config_set_wifi(ssid, pass[0] ? pass : NULL);
        if (err == ESP_OK) {
            printf("OK (Wi-Fi выставлен, выполните 'save' для сохранения в NVS)\n");
        } else {
            printf("Ошибка: неверный SSID или пароль\n");
        }
    } else if (strncmp(cmd, "set_token ", 10) == 0) {
        const char *tok = cmd + 10;
        while (*tok == ' ') tok++;
        esp_err_t err = kouki_config_set_token(tok);
        if (err == ESP_OK) {
            printf("OK (токен выставлен, выполните 'save' для сохранения в NVS)\n");
        } else {
            printf("Ошибка: токен должен быть от %d до %d символов\n",
                   KOUKI_CONFIG_MIN_TOKEN_LEN, KOUKI_CONFIG_MAX_TOKEN_LEN);
        }
    } else if (strncmp(cmd, "set_url ", 8) == 0) {
        const char *url = cmd + 8;
        while (*url == ' ') url++;
        esp_err_t err = kouki_config_set_url(url);
        if (err == ESP_OK) {
            printf("OK (URL выставлен, выполните 'save' для сохранения в NVS)\n");
        } else {
            printf("Ошибка: URL должен начинаться с http:// или https://\n");
        }
    } else if (strncmp(cmd, "set_source ", 11) == 0) {
        const char *src = cmd + 11;
        while (*src == ' ') src++;
        esp_err_t err = kouki_config_set_source(src);
        if (err == ESP_OK) {
            printf("OK (источник выставлен, выполните 'save' для сохранения в NVS)\n");
        } else {
            printf("Ошибка: источник слишком длинный\n");
        }
    } else if (strcmp(cmd, "save") == 0) {
        esp_err_t err = kouki_config_save();
        if (err == ESP_OK) {
            printf("OK (сохранено в NVS)\n");
        } else {
            printf("Ошибка сохранения (%s). Проверьте правильность через 'show'\n", esp_err_to_name(err));
        }
    } else if (strcmp(cmd, "reset") == 0) {
        kouki_config_erase();
        printf("OK (настройки очищены из NVS)\n");
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("Перезагрузка…\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        printf("Неизвестная команда. Введите 'help' для списка команд.\n");
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[512];

    printf("\n=== Kouki Station Console ===\n");
    printf("Введите 'help' для списка команд.\n> ");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len > 0) {
            handle_console_command(line);
        }
        printf("> ");
        fflush(stdout);
    }
    vTaskDelete(NULL);
}

esp_err_t kouki_setup_console_init(void)
{
    BaseType_t res = xTaskCreate(console_task, "kouki.console", 4096, NULL, 3, NULL);
    return res == pdPASS ? ESP_OK : ESP_FAIL;
}
