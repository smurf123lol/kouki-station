#pragma once

/*
 * Заглушка esp_ota_ops.h для host-стенда Ota (T121 П8).
 *
 * Повторяет только то, чем пользуется app_ota.cpp: активный раздел, его
 * состояние у bootloader (PENDING_VERIFY после OTA) и подтверждение образа.
 * Состояние раздела и слот задаёт тест.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_OTA_IMG_NEW = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_ABORTED = 4,
    ESP_OTA_IMG_UNDEFINED = 0xFF,
} esp_ota_img_states_t;

typedef struct {
    uint32_t address;
    uint32_t size;
    const char *label;
} esp_partition_t;

const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

#ifdef __cplusplus
}
#endif
