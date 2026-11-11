#pragma once

/*
 * Как и в ESP-IDF, esp_log.h первым делом тянет sdkconfig.h: модули берут оттуда
 * CONFIG_KOUKI_*. Стенд подаёт speech/config/sdkconfig.h (те же значения, что в
 * символьной сборке), иначе CONFIG_KOUKI_ALLOW_HTTP остался бы неопределён и
 * config отклонял бы http://.
 */
#include "sdkconfig.h"

#include <stdio.h>

/* Логи по умолчанию выключены; с -DKOUKI_TEST_LOG_CAPTURE их можно
 * перехватить и проверить текст (см. log_macro.h и log_capture.h). */
#include "log_macro.h"
