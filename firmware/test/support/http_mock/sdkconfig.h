#pragma once

/*
 * Макросы Kconfig, которые видят app_http.cpp и app_config.cpp на стенде Http
 * (вариант «HTTP разрешён»). Значения — из символьной сборки
 * tools/build-linux.sh --sim; набор шире, чем нужно Http, потому что стенд
 * линкует настоящий app_config.cpp (нужен kouki::config() для pinned CA).
 */
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_ALLOW_HTTP 1
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
