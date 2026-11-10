#pragma once

/*
 * Kconfig-символы, которые видят app_tone.cpp / app_audio_out.cpp на стенде
 * T121 П5 (вариант по умолчанию: сигналы включены, громкость 50 %).
 * Значения — из символьной сборки tools/build-linux.sh --sim --sim-mic.
 */
#define CONFIG_KOUKI_SOUND_SIGNALS 1
#define CONFIG_KOUKI_SIGNAL_VOLUME 50
#define CONFIG_KOUKI_SILENT_MODE 0
