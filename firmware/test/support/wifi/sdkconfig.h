#pragma once

/*
 * Единственный набор CONFIG_* на единицу трансляции.
 *
 * Заголовки -I перекрывают друг друга (у каждого стенда свой sdkconfig.h), а
 * `#include "sdkconfig.h"` из вложенного заглушечного esp_log.h ищет файл
 * сначала в СВОЁМ каталоге — поэтому в один TU могли попасть два разных
 * набора, и выигрывал не тот. Гвард делает так, что действует ПЕРВЫЙ
 * подключённый набор, а остальные молча выключаются: стенд задаёт нужный
 * конфиг через -include или порядок -I (находка ревью П11, T121).
 */
#ifndef KOUKI_TEST_SDKCONFIG_SEEN
#define KOUKI_TEST_SDKCONFIG_SEEN

/*
 * Макросы Kconfig, которые видят app_wifi.cpp и app_config.cpp на стенде Wifi.
 *
 * Значения совпадают с символьной сборкой (tools/build-linux.sh --sim --sim-mic):
 * тишина выключена, HTTP разрешён, речь включена, таймауты — как в
 * sdkconfig.defaults прошивки. Набор шире, чем нужно самому Wi-Fi, чтобы один
 * каталог заглушек обслуживал и app_config.cpp в этой же сборке.
 */
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_ALLOW_HTTP 1
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
#define CONFIG_KOUKI_WIFI_TIMEOUT_S 25

#endif  // KOUKI_TEST_SDKCONFIG_SEEN
