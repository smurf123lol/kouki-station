/*
 * Заглушки соседних модулей для host-стенда Setup (T121 П11), gcc как C.
 *
 * app_setup.cpp на портале и в консоли зовёт UI (экран настройки), голосовой
 * автомат (record/stop/ask/state/тестовые команды) и историю (тестовая
 * засыпка). Их настоящие реализации в стенд не линкуются: они тянут board,
 * LVGL и I2S. Здесь только счётчики — тест проверяет, что команда дошла до
 * нужной точки стыка.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "app_history.h"
#include "app_ui.h"
#include "app_voice.h"
#include "setup_stubs.h"

static int s_ui_setup_calls;
static char s_ui_setup_ssid[64];
static char s_ui_setup_url[64];

static int s_voice_print_state_calls;
static int s_voice_start_calls;
static int s_voice_stop_calls;
static int s_voice_mock_done_calls;
static int s_voice_last_session = -1;

static int s_history_clear_calls;
static int s_history_push_calls;
static size_t s_history_count_value;

void setup_stubs_reset(void)
{
    s_ui_setup_calls = 0;
    s_ui_setup_ssid[0] = '\0';
    s_ui_setup_url[0] = '\0';
    s_voice_print_state_calls = 0;
    s_voice_start_calls = 0;
    s_voice_stop_calls = 0;
    s_voice_mock_done_calls = 0;
    s_voice_last_session = -1;
    s_history_clear_calls = 0;
    s_history_push_calls = 0;
    s_history_count_value = 0;
}

int setup_ui_setup_calls(void) { return s_ui_setup_calls; }
const char *setup_ui_setup_ssid(void) { return s_ui_setup_ssid; }
const char *setup_ui_setup_url(void) { return s_ui_setup_url; }
int setup_voice_print_state_calls(void) { return s_voice_print_state_calls; }
int setup_voice_start_calls(void) { return s_voice_start_calls; }
int setup_voice_stop_calls(void) { return s_voice_stop_calls; }
int setup_voice_mock_done_calls(void) { return s_voice_mock_done_calls; }
int setup_voice_last_session(void) { return s_voice_last_session; }
int setup_history_clear_calls(void) { return s_history_clear_calls; }
int setup_history_push_calls(void) { return s_history_push_calls; }

/* ------------------------------------------------------------------- UI -- */

void kouki_ui_show_setup(const char *ssid, const char *url)
{
    s_ui_setup_calls++;
    snprintf(s_ui_setup_ssid, sizeof(s_ui_setup_ssid), "%s", ssid != NULL ? ssid : "");
    snprintf(s_ui_setup_url, sizeof(s_ui_setup_url), "%s", url != NULL ? url : "");
}

void kouki_ui_show_ota_progress(const char *text) { (void)text; }
void kouki_ui_clear_ota(void) {}

/* ---------------------------------------------------------------- voice -- */

void kouki_voice_print_state(void) { s_voice_print_state_calls++; }

esp_err_t kouki_voice_set_session(kouki_session_t session)
{
    s_voice_last_session = (int)session;
    return ESP_OK;
}

esp_err_t kouki_voice_start_recording(void)
{
    s_voice_start_calls++;
    return ESP_OK;
}

esp_err_t kouki_voice_stop_and_send(void)
{
    s_voice_stop_calls++;
    return ESP_OK;
}

esp_err_t kouki_voice_test_mock_done(const kouki_voice_result_t *result)
{
    (void)result;
    s_voice_mock_done_calls++;
    return ESP_OK;
}

/* -------------------------------------------------------------- history -- */

void kouki_history_clear(void)
{
    s_history_clear_calls++;
    s_history_count_value = 0;
}

void kouki_history_push(const kouki_voice_result_t *res)
{
    (void)res;
    s_history_push_calls++;
    if (s_history_count_value < KOUKI_HISTORY_CAPACITY) {
        s_history_count_value++;
    }
}

size_t kouki_history_count(void) { return s_history_count_value; }
