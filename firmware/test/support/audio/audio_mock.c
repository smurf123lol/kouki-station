#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_audio_out.h"
#include "audio_mock.h"
#include "esp_board_init.h"
#include "esp_err.h"

/*
 * Мок кодека станции для host-стенда T121 П5.
 *
 * Вместо bsp_audio_play запоминает, что и в каком порядке просили сыграть:
 * размер блока, первые сэмплы и признак «это тишина». Так стенд видит
 * настоящую очередь AudioOut и настоящий порядок воспроизведения, не поднимая
 * ни I2S, ни драйвер ES8311.
 */

#define MAX_CALLS 256

static audio_mock_call_t g_calls[MAX_CALLS];
static int g_calls_n = 0;
static int g_play_delay_us = 0;
static int g_fail_at_call = 0;
static int g_fail_code = ESP_FAIL;

int audio_mock_play_calls(void)
{
    return g_calls_n;
}

int audio_mock_total_bytes(void)
{
    int total = 0;
    for (int i = 0; i < g_calls_n && i < MAX_CALLS; i++) {
        total += g_calls[i].bytes;
    }
    return total;
}

const audio_mock_call_t *audio_mock_call(int index)
{
    if (index < 0 || index >= g_calls_n || index >= MAX_CALLS) {
        return NULL;
    }
    return &g_calls[index];
}

void audio_mock_set_delay_us(int delay_us)
{
    g_play_delay_us = delay_us;
}

void audio_mock_fail_at(int call_number, int code)
{
    g_fail_at_call = call_number;
    g_fail_code = code;
}

void audio_mock_reset(void)
{
    g_calls_n = 0;
}

esp_err_t esp_audio_play(const int16_t *data, int length, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;

    if (g_play_delay_us > 0) {
        struct timespec ts;
        ts.tv_sec = g_play_delay_us / 1000000;
        ts.tv_nsec = (long)(g_play_delay_us % 1000000) * 1000L;
        nanosleep(&ts, NULL);
    }

    if (g_fail_at_call > 0 && g_calls_n + 1 == g_fail_at_call) {
        return g_fail_code;
    }

    if (g_calls_n < MAX_CALLS) {
        audio_mock_call_t *call = &g_calls[g_calls_n];
        call->bytes = length;
        call->first[0] = (data != NULL && length >= 2) ? data[0] : 0;
        call->first[1] = (data != NULL && length >= 4) ? data[1] : 0;
        call->silence = true;
        if (data != NULL) {
            const uint8_t *raw = (const uint8_t *)data;
            const int probe = length < 640 ? length : 640;
            for (int i = 0; i < probe; i++) {
                if (raw[i] != 0) {
                    call->silence = false;
                    break;
                }
            }
        }
    }
    g_calls_n++;
    return ESP_OK;
}

esp_err_t esp_audio_get_play_vol(int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *volume = 50;
    return ESP_OK;
}

esp_err_t esp_audio_set_play_vol(int volume)
{
    (void)volume;
    return ESP_OK;
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_ERR_UNKNOWN";
    }
}
