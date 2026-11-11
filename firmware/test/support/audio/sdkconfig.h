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
 * Kconfig-символы, которые видят app_tone.cpp / app_audio_out.cpp на стенде
 * T121 П5 (вариант по умолчанию: сигналы включены, громкость 50 %).
 * Значения — из символьной сборки tools/build-linux.sh --sim --sim-mic.
 */
#define CONFIG_KOUKI_SOUND_SIGNALS 1
#define CONFIG_KOUKI_SIGNAL_VOLUME 50
#define CONFIG_KOUKI_SILENT_MODE 0

#endif  // KOUKI_TEST_SDKCONFIG_SEEN
