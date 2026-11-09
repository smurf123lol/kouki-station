#include "app_wake.h"
/*
 * Озвучивание ответа станции: POST /api/speech -> WAV 16 кГц моно -> кодек.
 *
 * Сервер синтезирует речь сам (sherpa-onnx, голос ru_RU-irina) и отдаёт
 * готовый WAV. Прошивке остаётся две вещи: забрать файл и развернуть моно
 * в стереопару L=R, потому что воспроизведение открыто в стерео
 * (main.c: esp_board_init(16000, 2, 16)), а bsp_audio_play трактует поток
 * как чередующиеся каналы — без разворота речь звучала бы вдвое быстрее.
 */
#include "app_speech.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "kouki.speech";

#if !defined(CONFIG_KOUKI_SPEECH_ENABLED)
/*
 * Озвучивание выключено в menuconfig: API остаётся, зависимости от HTTP
 * и кодека не компилируются вовсе (как у app_tone.c при выключенных сигналах).
 */
esp_err_t kouki_speech_play(const char *text)
{
    (void)text;
    ESP_LOGD(TAG, "озвучивание выключено (CONFIG_KOUKI_SPEECH_ENABLED)");
    return ESP_OK;
}

uint32_t kouki_speech_reserve(void)
{
    return 0;
}

esp_err_t kouki_speech_play_gen(const char *text, uint32_t gen)
{
    (void)text;
    (void)gen;
    return ESP_OK;
}

bool kouki_speech_busy(void)
{
    return false;
}

bool kouki_speech_worker_active(void)
{
    return false;
}

bool kouki_speech_is_playing(void)
{
    return false;
}

void kouki_speech_abort(void)
{
}

#else

#include <stdatomic.h>
#include "cJSON.h"
#include "app_audio_out.h"
#include "app_config.h"
#include "app_http.h"
#include "app_ota.h"
#include "app_voice.h"
#include "esp_board_init.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Сколько байт WAV готовы принять: 191 символ ответа — это ~13 с, т.е. ~420 КБ. */
#define SPEECH_READ_CAP (1200u * 1024u)
/* Стерео занимает вдвое больше: моно разворачиваем внутри того же буфера. */
#define SPEECH_BUF_CAP (2u * SPEECH_READ_CAP)

/*
 * Хвост тишины после речи: DMA проигрывает последний буфер ещё раз, и без
 * паузы ответ заканчивается повтором последнего слога (та же ловушка, что
 * описана в docs/decisions.md для сигналов). 4800 кадров = 0,3 с при 16 кГц;
 * буфер статический и нулевой, в PSRAM его держать незачем.
 */
#define SPEECH_TAIL_FRAMES 4800u
static int16_t s_tail[SPEECH_TAIL_FRAMES * 2u];

#define SPEECH_CHUNK_FRAMES 1600u /* 100 мс стерео (6400 Б) */

static atomic_uint_fast32_t s_latest_gen;
static atomic_uint_fast32_t s_aborted_up_to_gen;
static atomic_uint_fast32_t s_active_gen;
static atomic_uint_fast32_t s_speaking_gen;
static atomic_int s_active_workers;

bool kouki_speech_worker_active(void)
{
    return atomic_load_explicit(&s_active_workers, memory_order_acquire) > 0;
}

static bool speech_is_gen_aborted(uint32_t gen)
{
    const uint32_t latest = (uint32_t)atomic_load_explicit(&s_latest_gen, memory_order_acquire);
    return gen == 0 ||
           gen <= (uint32_t)atomic_load_explicit(&s_aborted_up_to_gen, memory_order_acquire) ||
           gen < latest;
}

uint32_t kouki_speech_reserve(void)
{
    return (uint32_t)atomic_fetch_add_explicit(&s_latest_gen, 1, memory_order_acq_rel) + 1u;
}

bool kouki_speech_busy(void)
{
    const uint32_t g = (uint32_t)atomic_load_explicit(&s_active_gen, memory_order_acquire);
    return g != 0 && !speech_is_gen_aborted(g);
}

bool kouki_speech_is_playing(void)
{
    const uint32_t g = (uint32_t)atomic_load_explicit(&s_speaking_gen, memory_order_acquire);
    return g != 0 && !speech_is_gen_aborted(g);
}

void kouki_speech_abort(void)
{
    const uint32_t latest = (uint32_t)atomic_load_explicit(&s_latest_gen, memory_order_acquire);
    uint_fast32_t cur = atomic_load_explicit(&s_aborted_up_to_gen, memory_order_relaxed);
    while (latest > (uint32_t)cur &&
           !atomic_compare_exchange_weak_explicit(&s_aborted_up_to_gen, &cur,
                                                   (uint_fast32_t)latest,
                                                   memory_order_release,
                                                   memory_order_relaxed)) {
    }
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t kouki_speech_wav_parse(const uint8_t *wav, size_t len, kouki_speech_wav_t *out)
{
    if (wav == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));

    if (len < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_fmt = false;
    size_t pos = 12;

    while (pos + 8 <= len) {
        const uint8_t *id = wav + pos;
        const uint32_t size = rd_le32(wav + pos + 4);
        const size_t body = pos + 8;

        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || body + 16 > len) {
                return ESP_ERR_INVALID_RESPONSE;
            }

            const uint16_t format = rd_le16(wav + body);
            out->channels = rd_le16(wav + body + 2);
            out->sample_rate = rd_le32(wav + body + 4);
            out->bits_per_sample = rd_le16(wav + body + 14);

            /* PCM без сжатия: mp3/aac станция не декодирует. */
            if (format != 1 || out->channels == 0 || out->channels > 2 || out->bits_per_sample != 16) {
                return ESP_ERR_NOT_SUPPORTED;
            }

            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) {
                return ESP_ERR_INVALID_RESPONSE;
            }

            const size_t avail = len - body;
            if ((size_t)size > avail) {
                return ESP_ERR_INVALID_RESPONSE;   /* size больше файла: не прыгаем вперёд */
            }
            out->data_offset = body;
            out->data_bytes = (size_t)size < avail ? (size_t)size : avail;
            return out->data_bytes == 0 ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
        }

        /* Чанки выровнены по чётной границе; size может быть любым 32-битным. */
        const size_t step = (size_t)size + (size & 1u);
        if (step >= len - body) {
            return ESP_ERR_INVALID_RESPONSE;       /* чанк съел весь файл — дальше некуда */
        }
        pos = body + step;
    }

    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t kouki_speech_stereo_expand(int16_t *samples, size_t frames)
{
    if (samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Разворот на месте и с конца: при записи пары (2i, 2i+1) читаем ещё
     * не тронутый samples[i], а всё, что левее i, уже прочитано раньше.
     */
    for (size_t i = frames; i > 0; i--) {
        const size_t src = i - 1;
        samples[src * 2] = samples[src];
        samples[src * 2 + 1] = samples[src];
    }

    return ESP_OK;
}

/* Тело запроса: только текст, без лишних полей — серверу больше ничего не нужно. */
static char *build_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "text", text);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

static esp_err_t speech_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        kouki_http_on_connected(kouki_config_get_srv_url());
    }
    return ESP_OK;
}

/*
 * Забирает WAV с сервера по чанкам в буфер PSRAM. Возвращает длину тела в *out_len.
 * Проверяет abort поколением между каждым этапом HTTP и между чанками чтения.
 */
static esp_err_t fetch_wav(const char *body, size_t body_len, uint32_t gen, uint8_t **out_buf, size_t *out_len)
{
    if (speech_is_gen_aborted(gen)) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *srv_url = kouki_config_get_srv_url();
    const char *token = kouki_config_get_srv_token();

    char url[256];
    snprintf(url, sizeof(url), "%s/api/speech", srv_url);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .buffer_size = 4096,
        .event_handler = speech_http_event,
    };

    esp_http_client_handle_t client = NULL;
    esp_err_t err = kouki_http_new_client(&cfg, &client);
    if (err != ESP_OK || client == NULL) {
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }

    if (speech_is_gen_aborted(gen)) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token != NULL && strlen(token) > 0) {
        char auth[300];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        char diag[128] = {0};
        kouki_http_diagnose_error(client, url, err, diag, sizeof(diag));
        ESP_LOGW(TAG, "запрос речи не прошёл: %s (%s)", esp_err_to_name(err), diag);
        esp_http_client_cleanup(client);
        return err;
    }

    size_t sent = 0;
    while (sent < body_len) {
        if (speech_is_gen_aborted(gen)) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_STATE;
        }
        const int wlen = esp_http_client_write(client, body + sent, (int)(body_len - sent));
        if (wlen <= 0) {
            ESP_LOGW(TAG, "запрос речи не ушёл (ошибка записи)");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        sent += (size_t)wlen;
    }

    if (speech_is_gen_aborted(gen)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_set_timeout_ms(client, 1000);
    int64_t hdr_len = -1;
    int hdr_wait_ms = 0;
    while (true) {
        if (speech_is_gen_aborted(gen)) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_ERR_INVALID_STATE;
        }
        hdr_len = esp_http_client_fetch_headers(client);
        if (hdr_len == -ESP_ERR_HTTP_EAGAIN) {
            hdr_wait_ms += 1000;
            if (hdr_wait_ms >= 15000) {
                break;
            }
            continue;
        }
        break;
    }
    const int status = esp_http_client_get_status_code(client);
    if (status > 0) {
        kouki_ota_mark_server_communicated();
    }
    if (hdr_len < 0) {
        char diag[128] = {0};
        kouki_http_diagnose_error(client, url, ESP_FAIL, diag, sizeof(diag));
        ESP_LOGW(TAG, "запрос речи не прошёл: %s", diag);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (speech_is_gen_aborted(gen)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_set_timeout_ms(client, 250);

    if (status != 200) {
        /* Тело ошибки короткое ({"error": "..."}) — читаем на стек без аллокации PSRAM. */
        char text[192] = {0};
        size_t pos = 0;
        int err_wait_ms = 0;
        while (pos < sizeof(text) - 1) {
            if (speech_is_gen_aborted(gen)) {
                break;
            }
            const int r = esp_http_client_read(client, text + pos, (int)(sizeof(text) - 1 - pos));
            if (r == -ESP_ERR_HTTP_EAGAIN) {
                err_wait_ms += 250;
                if (err_wait_ms >= 2000) {
                    break;
                }
                continue;
            }
            if (r <= 0) {
                break;
            }
            pos += (size_t)r;
            err_wait_ms = 0;
        }
        text[pos] = '\0';
        ESP_LOGW(TAG, "озвучивание недоступно: HTTP %d, %.160s", status, text);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (hdr_len > (int64_t)SPEECH_READ_CAP) {
        ESP_LOGW(TAG, "ответ речи слишком большой: %lld байт", (long long)hdr_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (speech_is_gen_aborted(gen)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_STATE;
    }

    size_t read_cap = SPEECH_READ_CAP;
    size_t alloc_cap = SPEECH_BUF_CAP;
    if (hdr_len > 0) {
        read_cap = (size_t)hdr_len;
        alloc_cap = read_cap * 2u + 64u;
        if (alloc_cap < 4096u) {
            alloc_cap = 4096u;
        }
        if (alloc_cap > SPEECH_BUF_CAP) {
            alloc_cap = SPEECH_BUF_CAP;
        }
    }

    uint8_t *buf = heap_caps_malloc(alloc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "нет памяти под WAV (%u байт)", (unsigned)alloc_cap);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    bool overflow = false;
    bool aborted = false;
    bool read_err = false;
    int read_wait_ms = 0;
    while (true) {
        if (speech_is_gen_aborted(gen)) {
            aborted = true;
            break;
        }
        if (len >= read_cap) {
            if (hdr_len > 0) {
                break;
            }
            char probe;
            const int r = esp_http_client_read(client, &probe, 1);
            if (r > 0) {
                overflow = true;
            } else if (r == -ESP_ERR_HTTP_EAGAIN) {
                read_wait_ms += 250;
                if (read_wait_ms >= 2000) {
                    break;
                }
                continue;
            } else if (r < 0) {
                read_err = true;
            }
            break;
        }
        const size_t avail = read_cap - len;
        const size_t chunk = avail < 4096u ? avail : 4096u;
        const int r = esp_http_client_read(client, (char *)(buf + len), (int)chunk);
        if (r > 0) {
            len += (size_t)r;
            read_wait_ms = 0;
        } else if (r == 0) {
            break;
        } else if (r == -ESP_ERR_HTTP_EAGAIN) {
            read_wait_ms += 250;
            if (read_wait_ms >= 15000) {
                read_err = true;
                break;
            }
        } else {
            read_err = true;
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (aborted) {
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }
    if (read_err) {
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (overflow || len == 0) {
        ESP_LOGW(TAG, "сервер прислал пустое или слишком большое тело (%u байт%s)",
                 (unsigned)len, overflow ? ", обрезано" : "");
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_buf = buf;
    *out_len = len;
    return ESP_OK;
}

static const char *get_next_phrase(const char *text, size_t *out_len, size_t max_chars)
{
    if (text == NULL || *text == '\0') {
        *out_len = 0;
        return NULL;
    }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    if (*text == '\0') {
        *out_len = 0;
        return NULL;
    }

    const char *p = text;
    const char *last_sentence_end = NULL;
    const char *last_clause_end = NULL;
    const char *last_space = NULL;
    size_t chars = 0;

    while (*p != '\0') {
        if ((*((const unsigned char *)p) & 0xC0) != 0x80) {
            chars++;
            if (chars > max_chars) {
                break;
            }
        }

        char c = *p;
        if (c == '.' || c == '!' || c == '?' || c == '\n' || c == ';') {
            bool is_num = (c == '.' && p > text && *(p - 1) >= '0' && *(p - 1) <= '9' && *(p + 1) >= '0' && *(p + 1) <= '9');
            if (!is_num) {
                last_sentence_end = p + 1;
                if (chars >= 30) {
                    break;
                }
            }
        } else if (c == ',' || c == ':' || c == '-') {
            last_clause_end = p + 1;
        } else if (c == ' ') {
            last_space = p;
        }

        p++;
    }

    if (*p == '\0' && chars <= max_chars) {
        *out_len = (size_t)(p - text);
        return text;
    }

    const char *end = NULL;
    if (last_sentence_end != NULL && (size_t)(last_sentence_end - text) > 0) {
        end = last_sentence_end;
    } else if (last_clause_end != NULL && (size_t)(last_clause_end - text) > 0 && chars >= max_chars * 2 / 3) {
        end = last_clause_end;
    } else if (last_space != NULL && (size_t)(last_space - text) > 0) {
        end = last_space;
    } else {
        end = p;
    }

    *out_len = (size_t)(end - text);
    return text;
}

static esp_err_t play_single_phrase(const char *phrase, size_t phrase_len, uint32_t gen, bool *out_played)
{
    if (speech_is_gen_aborted(gen)) {
        return ESP_ERR_INVALID_STATE;
    }

    const kouki_voice_state_t voice_state = kouki_voice_get_state();
    if (voice_state == KOUKI_VOICE_RECORDING || voice_state == KOUKI_VOICE_SENDING) {
        ESP_LOGI(TAG, "во время воспроизведения началась запись/отправка — прерываю");
        return ESP_ERR_INVALID_STATE;
    }

    char *phrase_str = malloc(phrase_len + 1);
    if (phrase_str == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(phrase_str, phrase, phrase_len);
    phrase_str[phrase_len] = '\0';

    char *body = build_body(phrase_str);
    free(phrase_str);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    const esp_err_t fetch_err = fetch_wav(body, strlen(body), gen, &wav, &wav_len);
    cJSON_free(body);

    if (fetch_err != ESP_OK) {
        ESP_LOGW(TAG, "запрос речи не удался: %s", esp_err_to_name(fetch_err));
        return fetch_err;
    }

    if (speech_is_gen_aborted(gen)) {
        free(wav);
        return ESP_ERR_INVALID_STATE;
    }

    if (kouki_voice_get_state() == KOUKI_VOICE_RECORDING || kouki_voice_get_state() == KOUKI_VOICE_SENDING) {
        ESP_LOGI(TAG, "после загрузки речи началась запись — прерываю");
        free(wav);
        return ESP_ERR_INVALID_STATE;
    }

    kouki_speech_wav_t info;
    esp_err_t err = kouki_speech_wav_parse(wav, wav_len, &info);
    if (err != ESP_OK || info.sample_rate != 16000) {
        ESP_LOGW(TAG, "неверный формат ответа WAV (err=%s, %u Гц)", esp_err_to_name(err), (unsigned)info.sample_rate);
        free(wav);
        return err != ESP_OK ? err : ESP_ERR_NOT_SUPPORTED;
    }

    int16_t *samples = (int16_t *)(wav + info.data_offset);
    const size_t frames = info.data_bytes / (sizeof(int16_t) * info.channels);
    if (frames > 0 && info.channels == 1) {
        err = kouki_speech_stereo_expand(samples, frames);
    }

    if (err == ESP_OK && frames > 0 && !speech_is_gen_aborted(gen)) {
        const size_t channels = info.channels == 1 ? 2u : (size_t)info.channels;
        size_t sent_frames = 0;

        while (sent_frames < frames) {
            if (speech_is_gen_aborted(gen)) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            if (kouki_voice_get_state() == KOUKI_VOICE_RECORDING || kouki_voice_get_state() == KOUKI_VOICE_SENDING) {
                ESP_LOGI(TAG, "во время воспроизведения началась запись — прерываю");
                err = ESP_ERR_INVALID_STATE;
                break;
            }

            const size_t rem = frames - sent_frames;
            const size_t chunk = rem < SPEECH_CHUNK_FRAMES ? rem : SPEECH_CHUNK_FRAMES;
            const int bytes = (int)(chunk * channels * sizeof(int16_t));

            kouki_audio_out_lock();
            if (speech_is_gen_aborted(gen)) {
                kouki_audio_out_unlock();
                err = ESP_ERR_INVALID_STATE;
                break;
            }
            atomic_store_explicit(&s_speaking_gen, (uint_fast32_t)gen, memory_order_release);
            err = esp_audio_play(samples + sent_frames * channels, bytes, portMAX_DELAY);
            uint_fast32_t exp_speak = (uint_fast32_t)gen;
            atomic_compare_exchange_strong_explicit(&s_speaking_gen, &exp_speak, 0,
                                                    memory_order_acq_rel, memory_order_relaxed);
            kouki_audio_out_unlock();

            if (err != ESP_OK) {
                break;
            }
            if (out_played != NULL) {
                *out_played = true;
            }
            sent_frames += chunk;
        }
    }

    free(wav);
    return err;
}

esp_err_t kouki_speech_play_gen(const char *text, uint32_t gen)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_OK;       /* нечего озвучивать: это не ошибка */
    }

    if (kouki_config_is_silent()) {
        ESP_LOGI(TAG, "режим тишины включен: воспроизведение речи пропущено");
        return ESP_OK;
    }

    atomic_fetch_add_explicit(&s_active_workers, 1, memory_order_acq_rel);
    esp_err_t err = ESP_OK;

    if (gen == 0) {
        gen = kouki_speech_reserve();
    }
    uint_fast32_t expected = 0;
    while (true) {
        if (speech_is_gen_aborted(gen)) {
            err = ESP_ERR_INVALID_STATE;
            goto worker_cleanup;
        }
        if (expected == 0 || speech_is_gen_aborted((uint32_t)expected)) {
            if (atomic_compare_exchange_weak_explicit(&s_active_gen, &expected,
                                                      (uint_fast32_t)gen,
                                                      memory_order_acq_rel,
                                                      memory_order_acquire)) {
                break;
            }
            continue;
        }
        ESP_LOGW(TAG, "предыдущее озвучивание ещё идёт — пропускаю");
        err = ESP_ERR_INVALID_STATE;
        goto worker_cleanup;
    }

    ESP_LOGI(TAG, "озвучиваю ответ: %.50s%s", text, strlen(text) > 50 ? "…" : "");

    const char *p = text;
    bool played_any = false;

    while (*p != '\0') {
        if (speech_is_gen_aborted(gen)) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t phrase_len = 0;
        const char *phrase_start = get_next_phrase(p, &phrase_len, 120);
        if (phrase_start == NULL || phrase_len == 0) {
            break;
        }
        p = phrase_start + phrase_len;

        err = play_single_phrase(phrase_start, phrase_len, gen, &played_any);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "озвучивание прервано или завершилось (%s)", esp_err_to_name(err));
            break;
        }
    }

    if (played_any && err == ESP_OK && !speech_is_gen_aborted(gen)) {
        /* Пауза после речи: закрывает хвост DMA и не даёт повтора */
        kouki_audio_out_lock();
        if (!speech_is_gen_aborted(gen)) {
            esp_audio_play(s_tail, (int)sizeof(s_tail), portMAX_DELAY);
        }
        kouki_audio_out_unlock();
    }

    uint_fast32_t exp_active = (uint_fast32_t)gen;
    atomic_compare_exchange_strong_explicit(&s_active_gen, &exp_active, 0,
                                            memory_order_acq_rel, memory_order_relaxed);

    /* Кулдаун 350 мс после речи для рассеивания реверберации — только при штатном завершении. */
    if (played_any && err == ESP_OK && !speech_is_gen_aborted(gen)) {
        kouki_wake_mute(800);
    }

worker_cleanup:
    atomic_fetch_sub_explicit(&s_active_workers, 1, memory_order_acq_rel);
    return err;
}

esp_err_t kouki_speech_play(const char *text)
{
    return kouki_speech_play_gen(text, 0);
}
#endif // CONFIG_KOUKI_SPEECH_ENABLED
