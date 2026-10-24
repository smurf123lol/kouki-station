/*
 * Запись голоса и отправка на сервер Kouki.
 *
 * Станция ничего не распознаёт сама: она записывает WAV 16 кГц моно 16 бит
 * и отправляет его в POST /api/voice. Так между прошивкой и сервером ровно
 * один контракт, и распознавание можно менять (whisper, sherpa, облако),
 * не перепрошивая плату.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KOUKI_VOICE_IDLE = 0,   /* ничего не происходит */
    KOUKI_VOICE_RECORDING,  /* пишем в буфер */
    KOUKI_VOICE_SENDING,    /* отправили WAV, ждём ответ */
    KOUKI_VOICE_DONE,       /* сервер ответил, разбираем результат */
    KOUKI_VOICE_ERROR,      /* сеть, токен или сервер ответили ошибкой */
} kouki_voice_state_t;

/* Сессия: что означает запись. Кнопка, а не догадка по словам. */
typedef enum {
    KOUKI_SESSION_TASK = 0, /* «Записать»: фраза становится задачей целиком */
    KOUKI_SESSION_ASK,      /* «Спросить»: фразу разбирает классификатор */
} kouki_session_t;

/* Всё, что станция показывает человеку после одной фразы. */
typedef struct {
    bool has_transcript;
    char transcript[192];   /* что услышал сервер */
    char intent[48];        /* add_task / list_tasks / complete_task / ask */
    char outcome[24];       /* created / executed / needs_review / failed */
    char task_title[128];   /* заголовок задачи, если она появилась */
    char task_status[24];
    char command[48];       /* имя исполненной команды */
    char answer[192];       /* ответ ассистента, если команда его вернула (ask) */
    char error[160];        /* текст ошибки для показа */
    int  http_status;       /* код ответа, 0 если запрос не дошёл */
    uint32_t duration_ms;   /* сколько длилась запись */
} kouki_voice_result_t;

typedef void (*kouki_voice_event_cb_t)(kouki_voice_state_t state,
                                       const kouki_voice_result_t *result,
                                       void *ctx);

/* Готовит буфер под запись (в PSRAM) и поднимает задачу записи. */
esp_err_t kouki_voice_init(kouki_voice_event_cb_t cb, void *ctx);

/*
 * Выбирает сессию до начала записи: «Записать» (intent=add_task) или «Спросить»
 * (intent не передаётся, фразу разбирает классификатор на сервере).
 */
esp_err_t kouki_voice_set_session(kouki_session_t session);

/* Начать запись. Повторный вызов во время записи ничего не меняет. */
esp_err_t kouki_voice_start_recording(void);

/* Остановить запись и отправить накопленное на сервер (асинхронно). */
esp_err_t kouki_voice_stop_and_send(void);

/* Диагностика: печатает уровень каждого канала входа. */
void kouki_voice_debug_levels(void);

bool kouki_voice_is_recording(void);

/* Текущее состояние — для отрисовки из LVGL-таймера. */
void kouki_voice_get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result);

#ifdef __cplusplus
}
#endif
