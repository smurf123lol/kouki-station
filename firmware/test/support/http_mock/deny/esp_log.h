#ifndef HTTP_MOCK_DENY_ESP_LOG_H
#define HTTP_MOCK_DENY_ESP_LOG_H

/*
 * Заглушка esp_log.h для варианта «HTTP запрещён» (CONFIG_KOUKI_ALLOW_HTTP=0).
 *
 * Нужна, чтобы sdkconfig.h брался из этого же каталога: config_mock/esp_log.h
 * подключает свой sdkconfig.h относительно себя, и -I порядок его не
 * перебивает. Логи в стенде выключены (if (0)), но вызовы компилируются.
 */
#include "sdkconfig.h"

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) do { if (0) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (0) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (0) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) printf("D %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#endif  // HTTP_MOCK_DENY_ESP_LOG_H
