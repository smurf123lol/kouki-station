/*
 * Заглушки соседних модулей для host-стенда Ota (T121 П8).
 *
 * app_ota.cpp проверяет «станция простаивает» четырьмя запросами к Voice и
 * Speech и показывает прогресс через UI. Настоящие модули для этого стенда не
 * нужны — их состояние задаёт тест, а вызовы UI только считаются.
 *
 * Компилируется как C (как и остальные заглушки стендов).
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "app_speech.h"
#include "app_ui.h"
#include "app_voice.h"
#include "esp_app_desc.h"

/* Состояние «станции» — его двигает тест через ota_stubs_* ниже. */
static kouki_voice_state_t s_voice_state = KOUKI_VOICE_IDLE;
static bool s_voice_recording = false;
static bool s_speech_busy = false;
static bool s_speech_playing = false;
static bool s_speech_worker = false;

static int s_ota_progress_calls = 0;
static int s_ota_clear_calls = 0;
static char s_ota_progress_text[128];

/* Сколько ещё опросов «занята ли станция» отдать прежнее состояние: так
 * проверяется ветка ожидания IDLE без хуков внутри мока. */
static int s_busy_queries_left = 0;

static esp_app_desc_t s_app_desc = {.version = "0.1.6"};

/* ---------------------------------------------------------- app_voice.h -- */

kouki_voice_state_t kouki_voice_get_state(void)
{
    if (s_busy_queries_left > 0) {
        s_busy_queries_left--;
        if (s_busy_queries_left == 0) {
            /* Станция «освободилась»: дальше отвечаем IDLE. */
            s_voice_state = KOUKI_VOICE_IDLE;
            s_voice_recording = false;
            s_speech_busy = false;
            s_speech_playing = false;
            s_speech_worker = false;
        }
    }
    return s_voice_state;
}

bool kouki_voice_is_recording(void)
{
    return s_voice_recording;
}

/* --------------------------------------------------------- app_speech.h -- */

bool kouki_speech_busy(void)
{
    return s_speech_busy;
}

bool kouki_speech_is_playing(void)
{
    return s_speech_playing;
}

bool kouki_speech_worker_active(void)
{
    return s_speech_worker;
}

/* ------------------------------------------------------------- app_ui.h -- */

void kouki_ui_show_ota_progress(const char *text)
{
    s_ota_progress_calls++;
    if (text != NULL) {
        strncpy(s_ota_progress_text, text, sizeof(s_ota_progress_text) - 1);
        s_ota_progress_text[sizeof(s_ota_progress_text) - 1] = '\0';
    } else {
        s_ota_progress_text[0] = '\0';
    }
}

void kouki_ui_clear_ota(void)
{
    s_ota_clear_calls++;
}

/* ------------------------------------------------------- esp_app_desc.h -- */

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_app_desc;
}

/* --------------------------------------------------- управление из теста -- */

void ota_stubs_reset(void)
{
    s_busy_queries_left = 0;
    s_voice_state = KOUKI_VOICE_IDLE;
    s_voice_recording = false;
    s_speech_busy = false;
    s_speech_playing = false;
    s_speech_worker = false;
    s_ota_progress_calls = 0;
    s_ota_clear_calls = 0;
    s_ota_progress_text[0] = '\0';
}

void ota_stubs_set_voice_state(int state, bool recording)
{
    s_voice_state = (kouki_voice_state_t)state;
    s_voice_recording = recording;
}

void ota_stubs_set_busy_queries(int queries)
{
    s_busy_queries_left = queries;
}

void ota_stubs_set_speech(bool busy, bool playing, bool worker)
{
    s_speech_busy = busy;
    s_speech_playing = playing;
    s_speech_worker = worker;
}

void ota_stubs_set_app_version(const char *version)
{
    if (version == NULL) {
        return;
    }
    strncpy(s_app_desc.version, version, sizeof(s_app_desc.version) - 1);
    s_app_desc.version[sizeof(s_app_desc.version) - 1] = '\0';
}

int ota_stubs_ota_progress_calls(void)
{
    return s_ota_progress_calls;
}

const char *ota_stubs_ota_progress_text(void)
{
    return s_ota_progress_text;
}

int ota_stubs_ota_clear_calls(void)
{
    return s_ota_clear_calls;
}
