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
 * Вариант «незащищённый HTTP запрещён» (CONFIG_KOUKI_ALLOW_HTTP=n, дефолт
 * Kconfig): тем же исходником app_http.cpp проверяется ветка отказа http://.
 * Остальные символы — как в разрешающем варианте, чтобы app_config.cpp
 * собирался тем же набором заголовков.
 */
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_ALLOW_HTTP 0
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10

#endif  // KOUKI_TEST_SDKCONFIG_SEEN
