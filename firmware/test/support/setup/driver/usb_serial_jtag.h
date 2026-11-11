#pragma once

/*
 * Заглушка driver/usb_serial_jtag.h для host-стенда Setup (T121 П11):
 * консоль USB-Serial-JTAG на хосте не поднимается, мок считает установки
 * драйвера и позволяет заказать сбой.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int tx_buffer_size;
    int rx_buffer_size;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() { .tx_buffer_size = 64, .rx_buffer_size = 256 }

esp_err_t usb_serial_jtag_driver_install(const usb_serial_jtag_driver_config_t *config);

void setup_jtag_reset(void);
void setup_jtag_fail_next_install(esp_err_t err);
int setup_jtag_install_calls(void);
int setup_jtag_vfs_calls(void);
int setup_jtag_rx_calls(void);
int setup_jtag_tx_calls(void);

#ifdef __cplusplus
}
#endif
