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

/* Логи по умолчанию выключены; с -DKOUKI_TEST_LOG_CAPTURE их можно
 * перехватить и проверить текст (см. log_macro.h и log_capture.h). */
#include "log_macro.h"

#endif  // HTTP_MOCK_DENY_ESP_LOG_H
