#pragma once

/*
 * Заглушка esp_mac.h для host-стенда Setup (T121 П11): имя SoftAP собирается
 * из MAC-адреса точки доступа. Мок отдаёт фиксированный адрес 24:6F:28:A1:BC:5E,
 * поэтому тест ждёт «Kouki-Setup-BC5E».
 */
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_MAC_WIFI_STA 0
#define ESP_MAC_WIFI_SOFTAP 1

esp_err_t esp_read_mac(uint8_t *mac, int type);

int setup_mac_calls_total(void);

#ifdef __cplusplus
}
#endif
