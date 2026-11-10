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
 * Вариант «громкость сигналов 0» (CONFIG_KOUKI_SIGNAL_VOLUME=0): тот же
 * app_tone.cpp проверяется на ветке «тон пропущен» — кодек не занимается.
 * Каталог quiet/ идёт первым в -I, поэтому его sdkconfig.h побеждает.
 */
#define CONFIG_KOUKI_SOUND_SIGNALS 1
#define CONFIG_KOUKI_SIGNAL_VOLUME 0
#define CONFIG_KOUKI_SILENT_MODE 0

#endif  // KOUKI_TEST_SDKCONFIG_SEEN
