#pragma once

/*
 * Заглушка driver/usb_serial_jtag_vfs.h для host-стенда Setup (T121 П11).
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LINE_ENDINGS_CRLF 0

esp_err_t usb_serial_jtag_vfs_use_driver(void);
esp_err_t usb_serial_jtag_vfs_set_rx_line_endings(int mode);
esp_err_t usb_serial_jtag_vfs_set_tx_line_endings(int mode);

#ifdef __cplusplus
}
#endif
