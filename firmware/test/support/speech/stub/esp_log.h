#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) do { if (0) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (0) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (0) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) printf("D %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
