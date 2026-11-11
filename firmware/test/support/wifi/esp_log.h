#pragma once

/*
 * Заглушка esp_log.h для стенда Wifi: подключает sdkconfig.h из этого же
 * каталога (esp_log.h в IDF первым делом включает sdkconfig.h), чтобы
 * app_wifi.cpp видел CONFIG_KOUKI_*.
 */
#include "sdkconfig.h"

#include <stdio.h>

/* Логи по умолчанию выключены; с -DKOUKI_TEST_LOG_CAPTURE их можно
 * перехватить и проверить текст (см. log_macro.h и log_capture.h). */
#include "log_macro.h"
