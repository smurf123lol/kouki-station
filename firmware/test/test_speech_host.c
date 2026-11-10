#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_speech.h"

extern int g_http_calls;
extern int g_http_status;
extern char g_http_url[256];
extern char g_http_auth[320];
extern char g_http_bodies[16][512];
extern int g_http_body_count;
extern int g_http_read_chunk_cap;
extern int g_http_read_calls;
extern int g_abort_on_read_call;
extern int g_http_hdr_eagain_left;
extern int g_http_hdr_calls;
extern int g_abort_on_hdr_call;
extern int g_http_read_eagain_left;
extern bool g_busy_seen_in_http;
extern bool g_playing_seen_in_http;

extern int g_large_alloc_calls;

extern int g_play_calls;
extern int g_play_bytes[128];
extern int16_t g_first_pcm[16];
extern int g_first_pcm_len;
extern int16_t *g_last_pcm;
extern int g_last_bytes;
extern int g_play_unlocked_errors;
extern int g_abort_on_play_call;
extern int g_play_delay_us;
extern int g_audio_calls_while_playing;
extern int g_audio_calls_while_busy;

extern int g_wake_mute_calls;
extern int g_last_wake_mute_ms;
extern int g_wake_mute_while_locked;

extern int g_audio_lock_calls;
extern int g_audio_unlock_calls;

void fake_speech_reset(void);
void fake_http_set_samples(const int16_t *samples, size_t count_i16);
void fake_http_set_format(int channels, int bits, int rate);
int kouki_audio_lock_depth(void);

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define CHECK(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
        exit(1); \
    } else { \
        g_tests_passed++; \
    } \
} while (0)

static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        size_t need = 0;
        if (*p < 0x80) need = 1;
        else if ((*p & 0xE0) == 0xC0) need = 2;
        else if ((*p & 0xF0) == 0xE0) need = 3;
        else if ((*p & 0xF8) == 0xF0) need = 4;
        else return false;
        for (size_t i = 1; i < need; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
        }
        p += need;
    }
    return true;
}

static size_t make_mono_wav(uint8_t *wav, const int16_t *samples, size_t frames)
{
    const size_t data_bytes = frames * sizeof(int16_t);
    memcpy(wav + 0, "RIFF", 4);
    wav[4] = (uint8_t)(36 + data_bytes);
    wav[5] = (uint8_t)((36 + data_bytes) >> 8);
    wav[6] = wav[7] = 0;
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    wav[16] = 16;
    wav[17] = wav[18] = wav[19] = 0;
    wav[20] = 1; wav[21] = 0;
    wav[22] = 1; wav[23] = 0;
    wav[24] = 0x80; wav[25] = 0x3E; wav[26] = wav[27] = 0;
    wav[28] = 0x00; wav[29] = 0x7D; wav[30] = wav[31] = 0;
    wav[32] = 2; wav[33] = 0;
    wav[34] = 16; wav[35] = 0;
    memcpy(wav + 36, "data", 4);
    wav[40] = (uint8_t)data_bytes;
    wav[41] = (uint8_t)(data_bytes >> 8);
    wav[42] = wav[43] = 0;
    memcpy(wav + 44, samples, data_bytes);
    return 44 + data_bytes;
}

static void test_wav_parse_and_stereo_expand(void)
{
    const int16_t samples[] = {1000, -1000, 2000};
    uint8_t wav[128];
    const size_t len = make_mono_wav(wav, samples, 3);
    kouki_speech_wav_t info;
    CHECK(kouki_speech_wav_parse(wav, len, &info) == ESP_OK, "WAV parse 16kHz mono");
    CHECK(info.data_offset == 44 && info.data_bytes == 6 && info.channels == 1 && info.sample_rate == 16000,
          "WAV fields match");

    wav[34] = 8;
    CHECK(kouki_speech_wav_parse(wav, len, &info) == ESP_ERR_NOT_SUPPORTED, "8-bit WAV rejected");
    uint8_t garbage[16] = {0};
    CHECK(kouki_speech_wav_parse(garbage, sizeof(garbage), &info) == ESP_ERR_INVALID_RESPONSE, "non-RIFF rejected");

    int16_t buf[6] = {100, 200, 300, 0, 0, 0};
    CHECK(kouki_speech_stereo_expand(buf, 3) == ESP_OK, "stereo_expand ok");
    CHECK(buf[0] == 100 && buf[1] == 100 && buf[2] == 200 && buf[3] == 200 && buf[4] == 300 && buf[5] == 300,
          "L=R expansion matches");
}

static void test_play_mono_stereo_and_refusals(void)
{
    fake_speech_reset();
    const int16_t samples[] = {1000, -1000, 2000};
    fake_http_set_samples(samples, 3);
    esp_err_t err = kouki_speech_play("Проверка голоса");
    CHECK(err == ESP_OK && g_http_calls == 1, "Single phrase play ok");
    CHECK(strstr(g_http_url, "/api/speech") != NULL, "URL contains /api/speech");
    CHECK(strstr(g_http_bodies[0], "\"text\":\"Проверка голоса\"") != NULL, "Body contains text");
    CHECK(strcmp(g_http_auth, "Bearer test-token") == 0, "Authorization Bearer header sent");
    CHECK(g_play_calls == 2 && g_play_bytes[0] == 12 && g_play_bytes[1] == 16000, "Mono expanded + 250ms tail");
    CHECK(g_first_pcm[0] == 1000 && g_first_pcm[1] == 1000 && g_first_pcm[2] == -1000, "First stereo L=R pairs match");
    CHECK(g_play_unlocked_errors == 0 && kouki_audio_lock_depth() == 0, "All esp_audio_play inside audio_out lock and unlocked on exit");
    CHECK(g_wake_mute_calls == 1 && g_last_wake_mute_ms == 800 && g_wake_mute_while_locked == 0,
          "kouki_wake_mute(800) called once outside audio_out lock");
    CHECK(g_busy_seen_in_http && !g_playing_seen_in_http,
          "kouki_speech_busy() true during HTTP while kouki_speech_is_playing() false");
    CHECK(g_audio_calls_while_playing == 1 && g_audio_calls_while_busy == 2,
          "is_playing() active on PCM chunk only, busy() active on both chunk and tail");
    CHECK(g_large_alloc_calls == 0, "Small Content-Length WAV skips 2.4MB PSRAM alloc");
    CHECK(!kouki_speech_is_playing() && !kouki_speech_busy(), "Not playing/busy after return");

    fake_speech_reset();
    const int16_t st[] = {10, 20, 30, 40};
    fake_http_set_samples(st, 4);
    fake_http_set_format(2, 16, 16000);
    CHECK(kouki_speech_play("стерео") == ESP_OK && g_play_calls == 2 && g_play_bytes[0] == 8,
          "Stereo passthrough not double-expanded");

    fake_speech_reset();
    kouki_config_set_silent(true);
    CHECK(kouki_speech_play("тишина") == ESP_OK && g_http_calls == 0 && g_play_calls == 0, "Silent skips HTTP & play");

    fake_speech_reset();
    g_http_status = 503;
    CHECK(kouki_speech_play("нет") == ESP_ERR_INVALID_RESPONSE && g_play_calls == 0 && g_large_alloc_calls == 0 && g_wake_mute_calls == 0,
          "HTTP 503 skips 2.4MB PSRAM alloc, plays nothing, no wake mute");

    fake_speech_reset();
    fake_http_set_samples(samples, 2);
    fake_http_set_format(1, 16, 22050);
    CHECK(kouki_speech_play("частота") == ESP_ERR_NOT_SUPPORTED && g_play_calls == 0, "22050 Hz rejected");
}

static void test_chunked_playback_and_phrase_splitting(void)
{
    fake_speech_reset();
    int16_t big[4000];
    for (int i = 0; i < 4000; i++) {
        big[i] = (int16_t)(i & 0xFF);
    }
    fake_http_set_samples(big, 4000);
    esp_err_t err = kouki_speech_play("Четыре тысячи сэмплов.");
    CHECK(err == ESP_OK, "4000-frame playback ok");
    CHECK(g_play_calls == 4 &&
          g_play_bytes[0] == 1600 * 4 &&
          g_play_bytes[1] == 1600 * 4 &&
          g_play_bytes[2] == 800 * 4 &&
          g_play_bytes[3] == 16000,
          "4000 frames split into 1600+1600+800 chunks + 250ms tail");
    CHECK(g_play_unlocked_errors == 0 && g_audio_lock_calls == 4 && g_audio_unlock_calls == 4,
          "Each chunk locked/unlocked individually around esp_audio_play");

    fake_speech_reset();
    const int16_t ping[] = {5, -5};
    fake_http_set_samples(ping, 2);
    const char *multi =
        "Первое предложение достаточно длинное для проверки нарезки по фразам в одном ответе. "
        "Второе предложение тоже имеет достаточную длину и корректный знак препинания в конце. "
        "Третье предложение завершает длинный кириллический ответ для синтеза речи.";
    CHECK(kouki_speech_play(multi) == ESP_OK, "Multi-phrase playback ok");
    CHECK(g_http_calls >= 2 && g_http_body_count >= 2, "Splits long text across multiple /api/speech requests");
    CHECK(g_play_calls >= 4, "Inter-phrase silence pauses inserted between chunks");
    for (int i = 0; i < g_http_body_count; i++) {
        CHECK(utf8_valid(g_http_bodies[i]), "Each phrase HTTP JSON body is valid UTF-8");
    }
}

static void usleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

static void *bg_slow_play_thread(void *arg)
{
    int *out_err = (int *)arg;
    *out_err = (int)kouki_speech_play("Длинная фоновая фраза для теста прерывания");
    return NULL;
}

static void test_generation_abort_and_supersede(void)
{
    // 1. Abort during chunked HTTP read
    fake_speech_reset();
    int16_t big[4000];
    memset(big, 0x11, sizeof(big));
    fake_http_set_samples(big, 4000);
    g_http_read_chunk_cap = 1024;
    g_abort_on_read_call = 2;
    CHECK(kouki_speech_play("Прервать на HTTP") == ESP_ERR_INVALID_STATE, "Abort mid-HTTP read returns INVALID_STATE");
    CHECK(g_play_calls == 0 && g_wake_mute_calls == 0 && kouki_audio_lock_depth() == 0,
          "Abort mid-HTTP plays nothing and does not mute wake");

    // 2. Abort during chunk #1 of TX playback (must stop before chunk #2 & skip tail & skip wake_mute)
    fake_speech_reset();
    fake_http_set_samples(big, 4000);
    g_abort_on_play_call = 1;
    CHECK(kouki_speech_play("Прервать на чанке 1") == ESP_ERR_INVALID_STATE, "Abort mid-TX returns INVALID_STATE");
    CHECK(g_play_calls == 1 && g_wake_mute_calls == 0 && kouki_audio_lock_depth() == 0,
          "Aborted TX stopped after chunk 1 without tail or wake_mute");
    CHECK(!kouki_speech_is_playing() && !kouki_speech_busy(), "Not playing/busy after abort");

    // 3. Immediate follow-up turn supersedes aborted previous generation concurrently!
    fake_speech_reset();
    fake_http_set_samples(big, 4000);
    g_play_delay_us = 15000; /* 15ms inside chunk 1 esp_audio_play */
    pthread_t th;
    int bg_err = 999;
    pthread_create(&th, NULL, bg_slow_play_thread, &bg_err);
    int tries = 0;
    while (!kouki_speech_is_playing() && tries++ < 500) {
        usleep_ms(1);
    }
    CHECK(kouki_speech_is_playing() && kouki_speech_busy(),
          "Background speech entered esp_audio_play chunk (both is_playing and busy true)");
    kouki_speech_abort();
    /* Do not mutate fake WAV globals while Thread A exits; Thread A already finished fetch_wav */
    const esp_err_t next_err = kouki_speech_play("Следующий ход");
    pthread_join(th, NULL);
    CHECK(bg_err == ESP_ERR_INVALID_STATE, "Background aborted speech finished with INVALID_STATE");
    CHECK(next_err == ESP_OK, "New speech generation succeeded immediately after aborting old generation");
    CHECK(g_play_unlocked_errors == 0 && kouki_audio_lock_depth() == 0, "Audio mutex balanced across threads");
    CHECK(g_wake_mute_calls == 1 && g_last_wake_mute_ms == 800 && g_wake_mute_while_locked == 0,
          "Only clean completing turn calls kouki_wake_mute(800) outside lock");

    // 4. Pre-worker kouki_speech_reserve() + abort BEFORE play_gen starts (fixes startup race)
    fake_speech_reset();
    fake_http_set_samples(big, 4000);
    const uint32_t reserved_aborted = kouki_speech_reserve();
    kouki_speech_abort();
    CHECK(kouki_speech_play_gen("Отменено до старта задачи", reserved_aborted) == ESP_ERR_INVALID_STATE,
          "Pre-worker reserve()+abort() rejects deferred play_gen");
    CHECK(g_http_calls == 0 && g_large_alloc_calls == 0 && g_play_calls == 0,
          "Pre-worker aborted gen skips HTTP, PSRAM alloc, and audio output");
    CHECK(!kouki_speech_is_playing() && !kouki_speech_busy(), "Idle after pre-worker abort");

    // 5. Newer kouki_speech_reserve() supersedes older unstarted generation even without abort()
    fake_speech_reset();
    fake_http_set_samples(big, 128);
    const uint32_t stale_gen = kouki_speech_reserve();
    const uint32_t newer_gen = kouki_speech_reserve();
    CHECK(kouki_speech_play_gen("Устаревшее поколение", stale_gen) == ESP_ERR_INVALID_STATE && g_http_calls == 0,
          "Superseded reserved generation rejected before HTTP");
    CHECK(kouki_speech_play_gen("Актуальное поколение", newer_gen) == ESP_OK && g_http_calls == 1 && g_play_calls == 2,
          "Latest reserved generation plays cleanly");

    // 6. Abort during slow /api/speech headers (-ESP_ERR_HTTP_EAGAIN loop)
    fake_speech_reset();
    fake_http_set_samples(big, 4000);
    g_http_hdr_eagain_left = 5;
    g_abort_on_hdr_call = 2;
    CHECK(kouki_speech_play("Зависли заголовки") == ESP_ERR_INVALID_STATE,
          "Abort wakes up inside header EAGAIN wait loop");
    CHECK(g_http_hdr_calls == 2 && g_large_alloc_calls == 0 && g_play_calls == 0 && !kouki_speech_busy(),
          "Header wait abort stops cleanly after 2nd poll without PSRAM alloc");

    // 7. Abort during slow /api/speech body (-ESP_ERR_HTTP_EAGAIN loop)
    fake_speech_reset();
    fake_http_set_samples(big, 4000);
    g_http_read_eagain_left = 5;
    g_abort_on_read_call = 2;
    CHECK(kouki_speech_play("Зависло тело") == ESP_ERR_INVALID_STATE,
          "Abort wakes up inside body read EAGAIN wait loop");
    CHECK(g_http_read_calls == 2 && g_play_calls == 0 && !kouki_speech_busy(),
          "Body EAGAIN wait abort stops cleanly after 2nd poll");

    // 8. Tracking kouki_speech_worker_active()
    fake_speech_reset();
    CHECK(!kouki_speech_worker_active(), "worker inactive initially");
    fake_http_set_samples(big, 4000);
    g_play_delay_us = 10000;
    pthread_t act_th;
    int act_err = 0;
    pthread_create(&act_th, NULL, bg_slow_play_thread, &act_err);
    tries = 0;
    while (!kouki_speech_worker_active() && tries++ < 500) {
        usleep_ms(1);
    }
    CHECK(kouki_speech_worker_active(), "kouki_speech_worker_active() true while running");
    kouki_speech_abort();
    pthread_join(act_th, NULL);
    CHECK(!kouki_speech_worker_active(), "kouki_speech_worker_active() false after thread join");
}

int main(void)
{
    printf("=== T116 Speech Hostcheck (real app_speech.c + locks + abort gen) ===\n");
    test_wav_parse_and_stereo_expand();
    test_play_mono_stereo_and_refusals();
    test_chunked_playback_and_phrase_splitting();
    test_generation_abort_and_supersede();
    printf("RESULTS: %d/%d checks passed (0 failures)\n", g_tests_passed, g_tests_run);
    return 0;
}
