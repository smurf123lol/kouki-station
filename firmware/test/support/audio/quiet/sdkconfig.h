#pragma once

/*
 * Вариант «громкость сигналов 0» (CONFIG_KOUKI_SIGNAL_VOLUME=0): тот же
 * app_tone.cpp проверяется на ветке «тон пропущен» — кодек не занимается.
 * Каталог quiet/ идёт первым в -I, поэтому его sdkconfig.h побеждает.
 */
#define CONFIG_KOUKI_SOUND_SIGNALS 1
#define CONFIG_KOUKI_SIGNAL_VOLUME 0
#define CONFIG_KOUKI_SILENT_MODE 0
