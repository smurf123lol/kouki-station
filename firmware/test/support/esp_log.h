#pragma once

#include <cstdio>

#define ESP_LOGE(tag, fmt, ...) do { if (0) std::printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) do { if (0) std::printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, fmt, ...) do { if (0) std::printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGD(tag, fmt, ...) do { if (0) std::printf("D %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
