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

/* Логи по умолчанию выключены; с -DKOUKI_TEST_LOG_CAPTURE их можно
 * перехватить и проверить текст (см. log_macro.h и log_capture.h). */
#include "log_macro.h"
