#ifndef SETUP_MOCK_H
#define SETUP_MOCK_H

/*
 * Управление моками host-стенда Setup (T121 П11).
 *
 * Один заголовок на все подмены стенда: httpd (esp_http_server.h), задачи
 * (freertos/task.h), USB-Serial-JTAG (driver/usb_serial_jtag*.h), MAC
 * (esp_mac.h) и память (esp_heap_caps.h). Сами функции живут в setup_mock.c,
 * который собирается gcc как C.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Сброс всех моков стенда перед тестом/сценарием. */
void setup_mocks_reset(void);

/* --- httpd ------------------------------------------------------------- */
void setup_httpd_reset(void);
void setup_httpd_fail_next_start(esp_err_t err);
int setup_httpd_start_calls(void);
int setup_httpd_register_calls(void);
const char *setup_httpd_route_uri(int index);
int setup_httpd_route_method(int index);
void *setup_httpd_route_user_ctx(int index);
int setup_httpd_last_status(void);
const char *setup_httpd_last_body(void);
const char *setup_httpd_last_type(void);
int setup_httpd_send_err_calls(void);
int setup_httpd_last_err_status(void);

/* --- задачи ------------------------------------------------------------ */
void setup_tasks_reset(void);
void setup_task_fail_next_create(void);
int setup_task_creates(void);
uint32_t setup_task_last_stack(void);
unsigned setup_task_last_priority(void);
const char *setup_task_last_name(void);
bool setup_task_run_last(void);
uint64_t setup_task_delay_ticks(void);
int setup_task_delay_calls(void);
int setup_task_deletes(void);

/* --- USB-Serial-JTAG --------------------------------------------------- */
void setup_jtag_reset(void);
void setup_jtag_fail_next_install(esp_err_t err);
int setup_jtag_install_calls(void);
int setup_jtag_vfs_calls(void);
int setup_jtag_rx_calls(void);
int setup_jtag_tx_calls(void);

/* --- MAC и перезагрузка ------------------------------------------------ */
int setup_mac_calls_total(void);
int setup_restart_calls(void);
void setup_restart_reset(void);

#ifdef __cplusplus
}
#endif

#endif  // SETUP_MOCK_H
