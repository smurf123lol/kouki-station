#pragma once

/*
 * Конфигурация сборки для host-стенда Setup (T121 П11).
 *
 * Значения как в реальной sim-сборке (build-t121-sim/sdkconfig): HTTP
 * разрешён (портал настройки живёт на 192.168.4.1 и по http тоже), отладочные
 * команды консоли включены сценарием sim, речь включена.
 */
#define CONFIG_KOUKI_ALLOW_HTTP 1
#define CONFIG_KOUKI_SILENT_MODE 0
#define CONFIG_KOUKI_SPEECH_ENABLED 1
#define CONFIG_KOUKI_TLS_TIMEOUT_S 10
#define CONFIG_KOUKI_TEST_CONSOLE_CMDS 1
#define CONFIG_KOUKI_WAKE_VERIFY 1
#define CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS 4000
