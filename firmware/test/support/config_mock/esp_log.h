#ifndef CONFIG_MOCK_ESP_LOG_H
#define CONFIG_MOCK_ESP_LOG_H

/*
 * Заглушка esp_log.h для стенда конфигурации.
 *
 * В образе ESP-IDF esp_log.h первым делом включает sdkconfig.h — оттуда
 * модули берут CONFIG_KOUKI_*. Здесь то же самое: стенд подаёт свой
 * config/sdkconfig.h, поэтому app_config.cpp видит те же макросы, что в сборке.
 */
#include "sdkconfig.h"

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) do { if (0) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (0) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (0) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) printf("D %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#endif  // CONFIG_MOCK_ESP_LOG_H
