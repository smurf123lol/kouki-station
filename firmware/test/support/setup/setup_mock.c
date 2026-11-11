/*
 * Мок httpd/USB-serial/esp_mac/heap для host-стенда Setup (T121 П11),
 * см. esp_http_server.h. Компилируется gcc как C.
 */
#include "setup_mock.h"

#include "esp_http_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"

#include <stdlib.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* ------------------------------------------------------------------ httpd -- */

#define MAX_ROUTES 8

typedef struct {
    char uri[32];
    int method;
    void *user_ctx;
} route_t;

static route_t s_routes[MAX_ROUTES];
static int s_route_count;
static int s_start_calls;
static int s_register_calls;
static esp_err_t s_fail_start;
static int s_server_obj;

static int s_last_status;
// Страница портала — несколько килобайт HTML: копия тела должна вмещать её
// целиком, иначе проверки на подстроки видят только первый килобайт.
static char s_last_body[16384];
static char s_last_type[64];
static int s_send_err_calls;
static int s_last_err_status;
/* Конфигурация, с которой позвали httpd_start: нужна, чтобы увидеть
 * `config.stack_size = 6144` из app_setup.cpp (пункт 31(g) ревью П11). */
static httpd_config_t s_last_config;

void setup_httpd_reset(void)
{
    s_route_count = 0;
    s_start_calls = 0;
    s_register_calls = 0;
    s_fail_start = ESP_OK;
    s_last_status = 0;
    s_last_body[0] = '\0';
    s_last_type[0] = '\0';
    s_send_err_calls = 0;
    s_last_err_status = 0;
    memset(&s_last_config, 0, sizeof(s_last_config));
}

void setup_httpd_fail_next_start(esp_err_t err) { s_fail_start = err; }

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    s_start_calls++;
    if (config != NULL) {
        s_last_config = *config;
    }
    if (s_fail_start != ESP_OK) {
        const esp_err_t err = s_fail_start;
        s_fail_start = ESP_OK;
        return err;
    }
    if (handle != NULL) {
        *handle = &s_server_obj;
    }
    return ESP_OK;
}

const httpd_config_t *setup_httpd_last_config(void)
{
    return &s_last_config;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    (void)handle;
    s_register_calls++;
    if (uri == NULL || s_route_count >= MAX_ROUTES) {
        return ESP_ERR_INVALID_ARG;
    }
    route_t *r = &s_routes[s_route_count++];
    snprintf(r->uri, sizeof(r->uri), "%s", uri->uri);
    r->method = (int)uri->method;
    r->user_ctx = uri->user_ctx;
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req == NULL || buf == NULL || buf_len == 0) {
        return -1;
    }
    /* Тело формы стенд кладёт в поле body (см. тест). */
    const char *body = req->body;
    if (body == NULL) {
        return -1;
    }
    const size_t len = strlen(body);
    size_t n = len < buf_len ? len : buf_len;
    memcpy(buf, body, n);
    return (int)n;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, ssize_t buf_len)
{
    (void)req;
    if (buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = strlen(buf);
    if (buf_len >= 0 && (size_t)buf_len < n) {
        n = (size_t)buf_len;
    }
    if (n >= sizeof(s_last_body)) {
        n = sizeof(s_last_body) - 1;
    }
    memcpy(s_last_body, buf, n);
    s_last_body[n] = '\0';
    return ESP_OK;
}

void setup_httpd_set_route_user_ctx(int index, void *ctx)
{
    if (index >= 0 && index < s_route_count) {
        s_routes[index].user_ctx = ctx;
    }
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, int status, const char *msg)
{
    (void)req;
    s_send_err_calls++;
    s_last_err_status = status;
    if (msg != NULL) {
        snprintf(s_last_body, sizeof(s_last_body), "%s", msg);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    if (status != NULL) {
        s_last_status = atoi(status);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req;
    if (type != NULL) {
        snprintf(s_last_type, sizeof(s_last_type), "%s", type);
    }
    return ESP_OK;
}

int setup_httpd_start_calls(void) { return s_start_calls; }
int setup_httpd_register_calls(void) { return s_register_calls; }

const char *setup_httpd_route_uri(int index)
{
    if (index < 0 || index >= s_route_count) {
        return "";
    }
    return s_routes[index].uri;
}

int setup_httpd_route_method(int index)
{
    if (index < 0 || index >= s_route_count) {
        return -1;
    }
    return s_routes[index].method;
}

void *setup_httpd_route_user_ctx(int index)
{
    if (index < 0 || index >= s_route_count) {
        return NULL;
    }
    return s_routes[index].user_ctx;
}

int setup_httpd_last_status(void) { return s_last_status; }
const char *setup_httpd_last_body(void) { return s_last_body; }
const char *setup_httpd_last_type(void) { return s_last_type; }
int setup_httpd_send_err_calls(void) { return s_send_err_calls; }
int setup_httpd_last_err_status(void) { return s_last_err_status; }

/* ------------------------------------------------------------------ esp_mac -- */

int setup_mac_calls;

esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    setup_mac_calls++;
    if (mac == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* MAC SoftAP: тест ждёт в имени сети байты 4 и 5 — BC:5E. */
    const uint8_t base[6] = {0x24, 0x6F, 0x28, 0xA1, 0xBC, 0x5E};
    memcpy(mac, base, sizeof(base));
    (void)type;
    return ESP_OK;
}

int setup_mac_calls_total(void) { return setup_mac_calls; }

/* ------------------------------------------------------------ usb_serial_jtag -- */

static int s_jtag_install_calls;
static int s_jtag_vfs_calls;
static int s_jtag_rx_calls;
static int s_jtag_tx_calls;
static esp_err_t s_fail_jtag_install;

void setup_jtag_reset(void)
{
    s_jtag_install_calls = 0;
    s_jtag_vfs_calls = 0;
    s_jtag_rx_calls = 0;
    s_jtag_tx_calls = 0;
    s_fail_jtag_install = ESP_OK;
}

void setup_jtag_fail_next_install(esp_err_t err) { s_fail_jtag_install = err; }
int setup_jtag_install_calls(void) { return s_jtag_install_calls; }
int setup_jtag_vfs_calls(void) { return s_jtag_vfs_calls; }

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config)
{
    (void)config;
    s_jtag_install_calls++;
    if (s_fail_jtag_install != ESP_OK) {
        const esp_err_t err = s_fail_jtag_install;
        s_fail_jtag_install = ESP_OK;
        return err;
    }
    return ESP_OK;
}

esp_err_t usb_serial_jtag_vfs_use_driver(void)
{
    s_jtag_vfs_calls++;
    return ESP_OK;
}

esp_err_t usb_serial_jtag_vfs_set_rx_line_endings(int mode)
{
    s_jtag_rx_calls++;
    (void)mode;
    return ESP_OK;
}

esp_err_t usb_serial_jtag_vfs_set_tx_line_endings(int mode)
{
    s_jtag_tx_calls++;
    (void)mode;
    return ESP_OK;
}

int setup_jtag_rx_calls(void) { return s_jtag_rx_calls; }
int setup_jtag_tx_calls(void) { return s_jtag_tx_calls; }

/* ------------------------------------------------------------------ restart -- */

static int s_restart_calls;

void setup_restart_reset(void) { s_restart_calls = 0; }
int setup_restart_calls(void) { return s_restart_calls; }

void esp_restart(void) { s_restart_calls++; }

/* ------------------------------------------------------------------- heap -- */

static int s_heap_calls;

void *setup_heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    s_heap_calls++;
    return malloc(size);
}

int setup_heap_calls(void) { return s_heap_calls; }

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 1234567;
}

size_t esp_get_free_heap_size(void)
{
    return 7654321;
}

/* ------------------------------------------------------------------- tasks -- */

typedef struct {
    void (*fn)(void *);
    void *arg;
    char name[24];
    uint32_t stack;
    unsigned priority;
} task_rec_t;

static task_rec_t s_task_rec;
static bool s_task_pending;
static bool s_task_fail;
static int s_task_creates;
static int s_task_deletes;
static uint64_t s_delay_ticks;
static int s_delay_calls;
static int s_task_obj;

void setup_tasks_reset(void)
{
    s_task_rec = (task_rec_t){0};
    s_task_pending = false;
    s_task_fail = false;
    s_task_creates = 0;
    s_task_deletes = 0;
    s_delay_ticks = 0;
    s_delay_calls = 0;
}

void setup_task_fail_next_create(void) { s_task_fail = true; }

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *out)
{
    if (s_task_fail) {
        s_task_fail = false;
        return pdFAIL;
    }
    s_task_creates++;
    s_task_rec.fn = task;
    s_task_rec.arg = arg;
    snprintf(s_task_rec.name, sizeof(s_task_rec.name), "%s", name != NULL ? name : "");
    s_task_rec.stack = stack;
    s_task_rec.priority = priority;
    s_task_pending = true;
    if (out != NULL) {
        *out = (TaskHandle_t)&s_task_obj;
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    s_delay_calls++;
    s_delay_ticks += ticks;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
    s_task_deletes++;
}

int setup_task_creates(void) { return s_task_creates; }
uint32_t setup_task_last_stack(void) { return s_task_rec.stack; }
unsigned setup_task_last_priority(void) { return s_task_rec.priority; }
const char *setup_task_last_name(void) { return s_task_rec.name; }
uint64_t setup_task_delay_ticks(void) { return s_delay_ticks; }
int setup_task_delay_calls(void) { return s_delay_calls; }
int setup_task_deletes(void) { return s_task_deletes; }

bool setup_task_run_last(void)
{
    if (!s_task_pending || s_task_rec.fn == NULL) {
        return false;
    }
    void (*fn)(void *) = s_task_rec.fn;
    void *arg = s_task_rec.arg;
    s_task_pending = false;
    fn(arg);
    return true;
}

/* ------------------------------------------------------------------ reset -- */

void setup_mocks_reset(void)
{
    setup_httpd_reset();
    setup_tasks_reset();
    setup_jtag_reset();
    setup_restart_reset();
}
