#pragma once

/*
 * Заглушка esp_system.h для host-стенда Ota (T121 П8): модуль после успешной
 * установки образа перезагружает плату. На стенде перезагрузка только
 * запоминается — тест проверяет, что она была запрошена.
 */
#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

/** Сколько раз просили перезагрузку. */
int ota_mock_restart_calls(void);

#ifdef __cplusplus
}
#endif
