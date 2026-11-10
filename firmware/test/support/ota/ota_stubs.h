#pragma once

/*
 * Соседние модули стенда Ota (T121 П8): состояние Voice/Speech, вызовы UI и
 * версия образа. Заглушки живут в ota_stubs.c; тест задаёт ими «станция
 * простаивает / занята» и видит, показывали ли прогресс OTA.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Сброс: IDLE, речь свободна, вызовы UI обнулены, версия 0.1.6. */
void ota_stubs_reset(void);

/** Состояние Voice (KOUKI_VOICE_IDLE и т. п.) и признак записи. */
void ota_stubs_set_voice_state(int state, bool recording);

/**
 * Сколько следующих опросов состояния Voice/Speech отдать «занято», после
 * чего станция «освобождается». Проверяет ветку ожидания IDLE: настоящий
 * цикл ждёт, пока не станет свободно, и без этого стенд завис бы.
 */
void ota_stubs_set_busy_queries(int queries);

/** Состояние Speech: busy / is_playing / worker_active. */
void ota_stubs_set_speech(bool busy, bool playing, bool worker);

/** Версия образа, которую вернёт esp_app_get_description(). */
void ota_stubs_set_app_version(const char *version);

/** Сколько раз показывали прогресс OTA и какой был последний текст. */
int ota_stubs_ota_progress_calls(void);
const char *ota_stubs_ota_progress_text(void);

/** Сколько раз очищали экран прогресса (при ошибке установки). */
int ota_stubs_ota_clear_calls(void);

#ifdef __cplusplus
}
#endif
