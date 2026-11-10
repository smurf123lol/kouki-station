#pragma once

/*
 * Заглушка esp_log.h для host-стенда Wake (T121 П9).
 *
 * Как и в IDF, первым делом подключает sdkconfig.h из этого же каталога: так
 * app_wake_verify.cpp видит CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS, а линкуемый в
 * этом же стенде app_config.cpp/app_http.cpp — свои CONFIG_KOUKI_*.
 */
#include "sdkconfig.h"

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) do { if (0) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (0) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (0) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) printf("D %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
