#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_audio_out.h"
#include "app_config.h"
#include "app_http.h"
#include "app_ota.h"
#include "app_speech.h"
#include "app_voice.h"
#include "app_wake.h"
#include "esp_board_init.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/task.h"

struct esp_http_client {
    int status;
    http_event_handle_cb handler;
    void *handler_arg;
};

int g_http_calls = 0;
int g_http_status = 200;
char g_http_url[256] = {0};
char g_http_auth[320] = {0};
char g_http_content_type[128] = {0};
char g_http_bodies[16][512];
int g_http_body_count = 0;
int g_http_read_chunk_cap = 4096;
int g_http_read_calls = 0;
int g_http_read_max_len = 0;
int g_voice_recording_on_read_call = 0;
int g_voice_recording_on_play_call = 0;
int g_abort_on_read_call = 0;
int g_http_hdr_eagain_left = 0;
int g_http_hdr_calls = 0;
int g_abort_on_hdr_call = 0;
int g_http_read_eagain_left = 0;
bool g_busy_seen_in_http = false;
bool g_playing_seen_in_http = false;

int g_large_alloc_calls = 0;
/* Пункт 27 ревью П10: размер последней PSRAM-аллокации и число вызовов
 * «сервер ответил» — иначе границы alloc_cap и mark_server_communicated
 * не наблюдаемы. */
long g_last_alloc_size = 0;
int g_server_communicated_calls = 0;

int g_play_calls = 0;
int g_audio_out_abort_calls = 0;
/* Наблюдаемость межфразовой/финальной паузы (п.28): длина и порядок запросов. */
int g_silence_calls = 0;
int g_silence_ms[16] = {0};
int g_play_bytes[128] = {0};
int16_t g_first_pcm[16] = {0};
int g_first_pcm_len = 0;
int16_t *g_last_pcm = NULL;
int g_last_bytes = 0;
int g_play_unlocked_errors = 0;
int g_abort_on_play_call = 0;
int g_play_delay_us = 0;
int g_audio_calls_while_playing = 0;
int g_audio_calls_while_busy = 0;

int g_wake_mute_calls = 0;
int g_last_wake_mute_ms = 0;
int g_wake_mute_while_locked = 0;

static pthread_mutex_t s_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int s_audio_lock_depth;
int g_audio_lock_calls = 0;
int g_audio_unlock_calls = 0;

kouki_voice_state_t g_stub_voice_state = KOUKI_VOICE_IDLE;

static uint8_t *s_wav_buf = NULL;
static size_t s_wav_cap = 0;
static size_t s_wav_len = 0;
static size_t s_wav_pos = 0;

static int16_t s_src_samples[16384];
static size_t s_src_frames = 0;
/* Синтетическое тело: сколько кадров дорисовать, не загружая их в массив.
 * Нужно для проверки ветки overflow (тело больше kReadCap = 1.2 МБ). */
size_t g_http_synth_frames = 0;
/* Подмена длины из fetch_headers: >0 — ровно столько байт, -1 — «неизвестно»
 * (chunked), 0 — как раньше, по фактическому WAV. */
int64_t g_http_hdr_len_override = 0;
/* Отдельный флаг: 0 — вернуть значение по умолчанию, 1 — отдать override как есть (в т.ч. 0). */
int g_http_hdr_len_override_set = 0;
static int s_src_channels = 1;
static int s_src_bits = 16;
static int s_src_rate = 16000;

void fake_speech_reset(void)
{
    g_http_calls = 0;
    g_http_status = 200;
    g_http_url[0] = '\0';
    g_http_auth[0] = '\0';
    g_http_content_type[0] = '\0';
    g_http_body_count = 0;
    memset(g_http_bodies, 0, sizeof(g_http_bodies));
    g_http_read_chunk_cap = 4096;
    g_http_read_calls = 0;
    g_http_read_max_len = 0;
    g_voice_recording_on_read_call = 0;
    g_abort_on_read_call = 0;
    g_http_hdr_eagain_left = 0;
    g_http_hdr_calls = 0;
    g_abort_on_hdr_call = 0;
    g_http_read_eagain_left = 0;
    g_busy_seen_in_http = false;
    g_playing_seen_in_http = false;

    g_large_alloc_calls = 0;
    g_last_alloc_size = 0;
    g_server_communicated_calls = 0;
    g_http_synth_frames = 0;
    g_http_hdr_len_override = 0;
    g_http_hdr_len_override_set = 0;

    g_play_calls = 0;
    memset(g_play_bytes, 0, sizeof(g_play_bytes));
    memset(g_first_pcm, 0, sizeof(g_first_pcm));
    g_first_pcm_len = 0;
    free(g_last_pcm);
    g_last_pcm = NULL;
    g_last_bytes = 0;
    g_play_unlocked_errors = 0;
    g_abort_on_play_call = 0;
    g_play_delay_us = 0;
    g_audio_calls_while_playing = 0;
    g_audio_calls_while_busy = 0;

    g_audio_out_abort_calls = 0;
    g_silence_calls = 0;
    memset(g_silence_ms, 0, sizeof(g_silence_ms));
    g_voice_recording_on_play_call = 0;
    g_wake_mute_calls = 0;
    g_last_wake_mute_ms = 0;
    g_wake_mute_while_locked = 0;

    g_audio_lock_calls = 0;
    g_audio_unlock_calls = 0;
    atomic_store(&s_audio_lock_depth, 0);

    g_stub_voice_state = KOUKI_VOICE_IDLE;
    s_src_frames = 0;
    s_src_channels = 1;
    s_src_bits = 16;
    s_src_rate = 16000;
    s_wav_len = 0;
    s_wav_pos = 0;

    /*
     * Адрес и токен сервера стенд задаёт через публичный C-фасад настоящего
     * kouki::Config: модуль речи читает их из членов класса, а не из моков.
     * Значения прежние — те, что раньше возвращали моки (URL и токен ровно
     * такие, какие ждут проверки t116).
     */
    kouki_config_set_silent(false);
    kouki_config_set_url("http://127.0.0.1:5300");
    kouki_config_set_token("test-token");
}

void fake_http_set_samples(const int16_t *samples, size_t count_i16)
{
    if (count_i16 > sizeof(s_src_samples) / sizeof(s_src_samples[0])) {
        count_i16 = sizeof(s_src_samples) / sizeof(s_src_samples[0]);
    }
    memcpy(s_src_samples, samples, count_i16 * sizeof(int16_t));
    s_src_frames = count_i16;
}

void fake_http_set_format(int channels, int bits, int rate)
{
    s_src_channels = channels;
    s_src_bits = bits;
    s_src_rate = rate;
    if (channels > 0) {
        s_src_frames = s_src_frames / (size_t)channels;
    }
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void build_wav(void)
{
    const size_t bytes_per_sample = (size_t)s_src_bits / 8u;
    size_t frames = s_src_frames;
    /* Синтетические кадры: тело нужного размера без 600k элементов в тесте. */
    if (g_http_synth_frames > frames) {
        frames = g_http_synth_frames;
    }
    const size_t data_bytes = frames * (size_t)s_src_channels * bytes_per_sample;
    const size_t need = 44u + data_bytes;
    if (need > s_wav_cap) {
        free(s_wav_buf);
        s_wav_buf = malloc(need);
        s_wav_cap = s_wav_buf ? need : 0;
    }
    s_wav_len = need;
    memcpy(s_wav_buf + 0, "RIFF", 4);
    put_le32(s_wav_buf + 4, (uint32_t)(36u + data_bytes));
    memcpy(s_wav_buf + 8, "WAVE", 4);
    memcpy(s_wav_buf + 12, "fmt ", 4);
    put_le32(s_wav_buf + 16, 16);
    put_le16(s_wav_buf + 20, 1);
    put_le16(s_wav_buf + 22, (uint16_t)s_src_channels);
    put_le32(s_wav_buf + 24, (uint32_t)s_src_rate);
    put_le32(s_wav_buf + 28, (uint32_t)(s_src_rate * s_src_channels * (int)bytes_per_sample));
    put_le16(s_wav_buf + 32, (uint16_t)(s_src_channels * bytes_per_sample));
    put_le16(s_wav_buf + 34, (uint16_t)s_src_bits);
    memcpy(s_wav_buf + 36, "data", 4);
    put_le32(s_wav_buf + 40, (uint32_t)data_bytes);
    if (data_bytes > 0) {
        const size_t loaded = s_src_frames * (size_t)s_src_channels * bytes_per_sample;
        const size_t copy = loaded < data_bytes ? loaded : data_bytes;
        if (copy > 0) {
            memcpy(s_wav_buf + 44, s_src_samples, copy);
        }
        /* Дорисовка синтетики: пила, чтобы данные были не нулевые. */
        for (size_t i = copy; i < data_bytes; i++) {
            s_wav_buf[44 + i] = (uint8_t)(i & 0xFF);
        }
    }
    s_wav_pos = 0;
}

/*
 * esp_err_to_name даёт общий мок config_mock/config_stubs.c (он же нужен
 * настоящему kouki::Config в этом стенде), поэтому своей копии здесь нет:
 * два определения в одной линковке — ошибка сборки.
 */

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    g_last_alloc_size = (long)size;
    if (size >= 1024u * 1024u) {
        g_large_alloc_calls++;
    }
    return malloc(size);
}

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

esp_err_t kouki_audio_out_init(void)
{
    return ESP_OK;
}

void kouki_audio_out_lock(void)
{
    pthread_mutex_lock(&s_audio_mutex);
    atomic_fetch_add(&s_audio_lock_depth, 1);
    g_audio_lock_calls++;
}

void kouki_audio_out_unlock(void)
{
    atomic_fetch_sub(&s_audio_lock_depth, 1);
    g_audio_unlock_calls++;
    pthread_mutex_unlock(&s_audio_mutex);
}

int kouki_audio_lock_depth(void)
{
    return atomic_load(&s_audio_lock_depth);
}

esp_err_t kouki_audio_play_silence(uint32_t silence_ms, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (silence_ms == 0) return ESP_OK;
    if (g_silence_calls < 16) {
        g_silence_ms[g_silence_calls] = (int)silence_ms;
    }
    g_silence_calls++;
    kouki_audio_out_lock();
    /* Тишина произвольной длины. Прошивка просит silence_ms * 64 байт
     * (стерео-буфер), а статический zeros[320] был всего 640 байт: на 250 мс
     * мок читал 16000 байт из 640-байтного массива. Обычный прогон этого не
     * видел, ASAN видел global-buffer-overflow (находка П14). Буфер выделяется
     * по фактическому размеру запроса. */
    const size_t total_bytes = (size_t)silence_ms * 64u;
    int16_t *zeros = (int16_t *)calloc(1, total_bytes);
    if (zeros != NULL) {
        esp_audio_play(zeros, (int)total_bytes, ticks_to_wait);
        free(zeros);
    }
    kouki_audio_out_unlock();
    return ESP_OK;
}

esp_err_t kouki_audio_play(const int16_t *data, int bytes, TickType_t ticks_to_wait)
{
    if (data == NULL || bytes <= 0) return ESP_OK;
    kouki_audio_out_lock();
    esp_err_t err = esp_audio_play(data, bytes, ticks_to_wait);
    kouki_audio_out_unlock();
    return err;
}

esp_err_t kouki_audio_play_with_tail(const int16_t *data, int bytes, uint32_t tail_ms, TickType_t ticks_to_wait)
{
    esp_err_t err = kouki_audio_play(data, bytes, ticks_to_wait);
    if (err == ESP_OK && tail_ms > 0) {
        err = kouki_audio_play_silence(tail_ms, ticks_to_wait);
    }
    return err;
}

esp_err_t kouki_audio_out_enqueue_pcm(int16_t *data, size_t bytes, bool transfer_ownership)
{
    esp_err_t err = kouki_audio_play(data, (int)bytes, portMAX_DELAY);
    if (transfer_ownership && data != NULL) free(data);
    return err;
}

esp_err_t kouki_audio_out_enqueue_silence(uint32_t silence_ms)
{
    return kouki_audio_play_silence(silence_ms, portMAX_DELAY);
}

void kouki_audio_out_abort(void)
{
    /* Пункт 29 ревью П10: abort — наблюдаемый эффект, а не пустая заглушка:
     * стенд проверяет, что Speech::abort действительно доходит до кодека. */
    g_audio_out_abort_calls++;
}

esp_err_t kouki_audio_out_wait_idle(TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    return ESP_OK;
}

bool kouki_audio_out_is_busy(void)
{
    return false;
}

void kouki_wake_mute(int duration_ms)
{
    if (atomic_load(&s_audio_lock_depth) != 0) {
        g_wake_mute_while_locked++;
    }
    g_wake_mute_calls++;
    g_last_wake_mute_ms = duration_ms;
}

/*
 * kouki_config_* здесь больше не подменяются: в стенд линкуется настоящий
 * kouki::Config (app_config.cpp) — модуль речи берёт адрес/токен из его
 * членов, поэтому стенд обязан подавать тот же объект, что и образ. Тишина и
 * сервер задаются через публичный C-фасад конфигурации (см. main()).
 */

void kouki_ota_mark_server_communicated(void)
{
    /* Пункт 27: вызов из ветки «сервер ответил» должен быть наблюдаем. */
    g_server_communicated_calls++;
}

bool kouki_voice_get_snapshot(kouki_voice_state_t *out_state, kouki_voice_result_t *out_result)
{
    if (out_state) *out_state = g_stub_voice_state;
    if (out_result) memset(out_result, 0, sizeof(*out_result));
    return true;
}

kouki_voice_state_t kouki_voice_get_state(void)
{
    return g_stub_voice_state;
}

esp_err_t kouki_voice_dismiss_error(void)
{
    return ESP_OK;
}

esp_err_t kouki_http_new_client(const esp_http_client_config_t *base_cfg, esp_http_client_handle_t *out_client)
{
    *out_client = esp_http_client_init(base_cfg);
    return *out_client ? ESP_OK : ESP_ERR_NO_MEM;
}

void kouki_http_on_connected(const char *url) { (void)url; }

void kouki_http_diagnose_error(esp_http_client_handle_t client, const char *url, esp_err_t req_err, char *err_buf, size_t max_len)
{
    (void)client;
    (void)url;
    (void)req_err;
    if (err_buf && max_len > 0) {
        snprintf(err_buf, max_len, "mock http error");
    }
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct esp_http_client *client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    g_http_calls++;
    client->status = g_http_status;
    client->handler = config ? config->event_handler : NULL;
    client->handler_arg = config ? config->user_data : NULL;
    if (config && config->url) {
        snprintf(g_http_url, sizeof(g_http_url), "%s", config->url);
    }
    return client;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    (void)client;
    if (key && strcmp(key, "Authorization") == 0 && value) {
        snprintf(g_http_auth, sizeof(g_http_auth), "%s", value);
    }
    if (key && strcmp(key, "Content-Type") == 0 && value) {
        snprintf(g_http_content_type, sizeof(g_http_content_type), "%s", value);
    }
    return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    (void)client;
    (void)write_len;
    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len)
{
    (void)client;
    if (g_http_body_count < 16) {
        const size_t copy = (size_t)len < sizeof(g_http_bodies[0]) - 1 ? (size_t)len : sizeof(g_http_bodies[0]) - 1;
        memcpy(g_http_bodies[g_http_body_count], buffer, copy);
        g_http_bodies[g_http_body_count][copy] = '\0';
        g_http_body_count++;
    }
    return len;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms)
{
    (void)client;
    (void)timeout_ms;
    return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    (void)client;
    g_http_hdr_calls++;
    if (kouki_speech_busy()) g_busy_seen_in_http = true;
    if (kouki_speech_is_playing()) g_playing_seen_in_http = true;
    if (g_abort_on_hdr_call > 0 && g_http_hdr_calls == g_abort_on_hdr_call) {
        kouki_speech_abort();
    }
    if (g_http_hdr_eagain_left > 0) {
        g_http_hdr_eagain_left--;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    build_wav();
    if (g_http_hdr_len_override_set != 0) {
        return g_http_hdr_len_override;
    }
    return g_http_status == 200 ? (int64_t)s_wav_len : 32;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    (void)client;
    return g_http_status;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    (void)client;
    g_http_read_calls++;
    if (len > g_http_read_max_len) {
        g_http_read_max_len = len;
    }
    if (g_voice_recording_on_read_call > 0 && g_http_read_calls == g_voice_recording_on_read_call) {
        /* Хук: посреди загрузки WAV стартовала запись — Speech обязан прервать play. */
        g_stub_voice_state = KOUKI_VOICE_RECORDING;
    }
    if (g_abort_on_read_call > 0 && g_http_read_calls == g_abort_on_read_call) {
        kouki_speech_abort();
    }
    if (g_http_read_eagain_left > 0) {
        g_http_read_eagain_left--;
        return -ESP_ERR_HTTP_EAGAIN;
    }
    if (g_http_status != 200) {
        const char *body = "{\"error\":\"Озвучивание не подключено.\"}";
        const size_t blen = strlen(body);
        if (s_wav_pos >= blen) return 0;
        const size_t left = blen - s_wav_pos;
        const size_t take = (size_t)len < left ? (size_t)len : left;
        memcpy(buffer, body + s_wav_pos, take);
        s_wav_pos += take;
        return (int)take;
    }
    if (s_wav_pos >= s_wav_len) {
        return 0;
    }
    size_t max_len = (size_t)len;
    if (max_len > (size_t)g_http_read_chunk_cap) {
        max_len = (size_t)g_http_read_chunk_cap;
    }
    const size_t left = s_wav_len - s_wav_pos;
    const size_t take = max_len < left ? max_len : left;
    memcpy(buffer, s_wav_buf + s_wav_pos, take);
    s_wav_pos += take;
    return (int)take;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client) { (void)client; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { free(client); return ESP_OK; }

esp_err_t esp_audio_play(const int16_t *data, int length, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    if (atomic_load(&s_audio_lock_depth) != 1) {
        g_play_unlocked_errors++;
    }
    if (kouki_speech_is_playing()) {
        g_audio_calls_while_playing++;
    }
    if (kouki_speech_busy()) {
        g_audio_calls_while_busy++;
    }
    if (g_play_calls < 128) {
        g_play_bytes[g_play_calls] = length;
    }
    if (g_play_calls == 0 && length > 0) {
        size_t n = (size_t)length / sizeof(int16_t);
        if (n > 16) n = 16;
        memcpy(g_first_pcm, data, n * sizeof(int16_t));
        g_first_pcm_len = (int)n;
    }
    g_play_calls++;
    if (g_voice_recording_on_play_call > 0 && g_play_calls == g_voice_recording_on_play_call) {
        /* Хук: запись стартовала посреди воспроизведения (п.28). */
        g_stub_voice_state = KOUKI_VOICE_RECORDING;
    }

    free(g_last_pcm);
    g_last_pcm = malloc((size_t)length);
    if (g_last_pcm && length > 0) {
        memcpy(g_last_pcm, data, (size_t)length);
    }
    g_last_bytes = length;

    if (g_abort_on_play_call > 0 && g_play_calls == g_abort_on_play_call) {
        kouki_speech_abort();
    }
    if (g_play_delay_us > 0) {
        struct timespec ts = {
            .tv_sec = g_play_delay_us / 1000000L,
            .tv_nsec = (g_play_delay_us % 1000000L) * 1000L,
        };
        nanosleep(&ts, NULL);
    }
    return ESP_OK;
}
