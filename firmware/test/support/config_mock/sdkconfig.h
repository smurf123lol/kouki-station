#pragma once

/*
 * Макросы Kconfig, которые видит app_config.cpp на стенде.
 *
 * Значения взяты из символьной сборки (tools/build-linux.sh --sim --sim-mic):
 * тишина выключена, HTTP разрешён, речь включена. Остальные символы модулю
 * конфигурации не нужны.
 */
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_ALLOW_HTTP 1
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
