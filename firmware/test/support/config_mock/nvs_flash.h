#ifndef CONFIG_MOCK_NVS_FLASH_H
#define CONFIG_MOCK_NVS_FLASH_H

/*
 * Заглушка nvs_flash.h: модуль конфигурации включает его, но не вызывает
 * nvs_flash_init/erase — этим занимается main.c. Стенд инициализации флеша
 * не требует: «флеш» мока живёт в памяти процесса.
 */
#include "esp_err.h"

#endif  // CONFIG_MOCK_NVS_FLASH_H
