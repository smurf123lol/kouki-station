#pragma once

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
