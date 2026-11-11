#pragma once

/*
 * Заглушка freertos/task.h для host-стенда Ota (T121 П8): app_ota.cpp
 * включает её как в IDF, а типы задачи приходят из freertos/FreeRTOS.h.
 */
#include "freertos/FreeRTOS.h"
