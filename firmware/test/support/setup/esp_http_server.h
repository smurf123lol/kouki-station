#pragma once

/*
 * Заглушка esp_http_server.h для host-стенда Setup (T121 П11).
 *
 * httpd-сервер на стенде не поднимается: мок запоминает запуск, таблицу
 * маршрутов и ответы, которые модуль отправил в портал. Так проверяются
 * обработчики (index_get/save_post) без сокетов: тест сам формирует
 * httpd_req_t с телом формы и смотрит, какой HTTP-код и текст ушли в ответ.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *httpd_handle_t;
typedef enum { HTTP_GET = 0, HTTP_POST = 1 } httpd_method_t;

/**
 * Запрос: тело формы и user_ctx — всё, что нужно обработчикам модуля.
 * body — поле мока (в IDF тела в структуре нет, его читают через
 * httpd_req_recv), поэтому телу отведено отдельное поле, а user_ctx остаётся
 * тем, что модуль прокидывает в обработчик.
 */
typedef struct httpd_req {
    int content_len;
    void *user_ctx;
    const char *body;
} httpd_req_t;

typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t *req);

typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_handler_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef struct {
    uint16_t port;
    uint16_t stack_size;
    uint16_t max_uri_handlers;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { .port = 80, .stack_size = 4096, .max_uri_handlers = 8 }

/** Значение httpd_resp_send()/send_err(): длина неизвестна, брать strlen. */
#define HTTPD_RESP_USE_STRLEN (-1)

#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_500_INTERNAL_SERVER_ERROR 500

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, int status, const char *msg);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);

/* --- управление моком (только стенд) ----------------------------------- */

void setup_httpd_reset(void);
/** Заказать сбой следующего httpd_start. */
void setup_httpd_fail_next_start(esp_err_t err);
int setup_httpd_start_calls(void);
int setup_httpd_register_calls(void);
/** Запомненные маршруты: uri/method/user_ctx. */
const char *setup_httpd_route_uri(int index);
int setup_httpd_route_method(int index);
void *setup_httpd_route_user_ctx(int index);
/** Последний ответ портала. */
int setup_httpd_last_status(void);
const char *setup_httpd_last_body(void);
const char *setup_httpd_last_type(void);
/** Сколько раз вызывали httpd_resp_send_err и с каким кодом. */
int setup_httpd_send_err_calls(void);
void setup_httpd_set_route_user_ctx(int index, void *ctx);
/** Конфигурация последнего httpd_start() (в т.ч. stack_size). */
const httpd_config_t *setup_httpd_last_config(void);
int setup_httpd_last_err_status(void);

#ifdef __cplusplus
}
#endif
