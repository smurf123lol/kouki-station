#pragma once

/*
 * Макросы Kconfig, которые видит app_wake_verify.cpp на стенде Wake (T121 П9).
 *
 * Значения — как в символьной сборке (tools/build-linux.sh --sim --sim-mic):
 * ASR-подтверждение включено, таймаут 4000 мс. Сам детектор от Kconfig не
 * зависит (включается через NVS wake_en), но флаги verify нужны, чтобы
 * app_wake_verify.cpp собирался без висячих макросов (AGENTS.md п.5).
 */
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_ALLOW_HTTP 1
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
#define CONFIG_KOUKI_WAKE_VERIFY 1
#define CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS 4000
