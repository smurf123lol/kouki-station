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

/* Логи по умолчанию выключены; с -DKOUKI_TEST_LOG_CAPTURE их можно
 * перехватить и проверить текст (см. log_macro.h и log_capture.h). */
#include "log_macro.h"

#endif  // CONFIG_MOCK_ESP_LOG_H
