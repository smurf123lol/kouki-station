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
 * Конфигурация сборки для host-стенда Setup (T121 П11).
 *
 * Значения как в реальной sim-сборке (tools/build-linux.sh --sim --sim-mic,
 * снимок _scratch/t121-sim/sdkconfig): HTTP разрешён (портал настройки живёт
 * на 192.168.4.1 и по http тоже), отладочные команды консоли включены
 * сценарием sim, речь включена. Остальные символы перечислены, потому что
 * этот заголовок подключается с -include к обоим TU стенда (test_setup.cpp и
 * app_setup.cpp) и должен быть единственным источником CONFIG_KOUKI_*:
 * иначе app_setup.cpp подтягивал support/wifi/sdkconfig.h и видел другой
 * набор макросов (находка ревью П11, T121).
 *
 * Значения сверены с `grep -E '^CONFIG_KOUKI_' _scratch/t121-sim/sdkconfig`;
 * при изменении Kconfig.projbuild этот файл надо править вместе с ним.
 */
#define CONFIG_KOUKI_ALLOW_HTTP 1
#define CONFIG_KOUKI_FOLLOWUP_MIN_VOICED_MS 500
#define CONFIG_KOUKI_FOLLOWUP_SILENCE_MS 1200
#define CONFIG_KOUKI_FOLLOWUP_WINDOW_MS 6000
#define CONFIG_KOUKI_MAX_RECORD_S 15
#define CONFIG_KOUKI_MIC_CHANNEL 1
#define CONFIG_KOUKI_MIC_GAIN 6
#define CONFIG_KOUKI_MIN_AMPLITUDE 120
#define CONFIG_KOUKI_PLAYER_PA_GPIO 15
#define CONFIG_KOUKI_PLAYER_VOLUME 80
#define CONFIG_KOUKI_SIGNAL_VOLUME 50
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_SOUND_SIGNALS 1
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TEST_CONSOLE_CMDS 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
#define CONFIG_KOUKI_WAKE_VERIFY 1
#define CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS 4000
#define CONFIG_KOUKI_WIFI_TIMEOUT_S 25

#endif  // KOUKI_TEST_SDKCONFIG_SEEN
