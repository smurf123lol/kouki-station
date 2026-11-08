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

bool kouki_speech_busy(void)
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

#include "cJSON.h"
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

static volatile bool s_busy;
static volatile bool s_is_playing;
static volatile bool s_abort_requested;

bool kouki_speech_busy(void)
{
    return s_busy;
}

bool kouki_speech_is_playing(void)
{
    return s_is_playing;
}

void kouki_speech_abort(void)
{
    s_abort_requested = true;
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
/* Сколько символов UTF-8 в строке (байты продолжения не считаем). */
static size_t __attribute__((unused)) utf8_chars(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/* Копия не длиннее max_chars символов UTF-8: символ не разрезаем. */
static char * __attribute__((unused)) utf8_prefix_copy(const char *s, size_t max_chars)
{
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *cut = p;
    size_t chars = 0;

    while (*p != '\0' && chars < max_chars) {
        p++;
        while ((*p & 0xC0) == 0x80) {
            p++;                       /* байты продолжения текущего символа */
        }
        chars++;
        cut = p;
    }

    const size_t len = (size_t)(cut - (const unsigned char *)s);
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

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

/* Приёмник тела ответа: копим байты в PSRAM до предела буфера. */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    bool overflow;
} speech_sink_t;

static esp_err_t speech_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        kouki_http_on_connected(kouki_config_get_srv_url());
    }

    speech_sink_t *sink = (speech_sink_t *)evt->user_data;
    if (sink == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        const size_t want = (size_t)evt->data_len;
        if (sink->len + want <= sink->cap) {
            memcpy(sink->buf + sink->len, evt->data, want);
            sink->len += want;
        } else {
            sink->overflow = true;
        }
    }

    return ESP_OK;
}

/*
 * Забирает WAV с сервера в буфер PSRAM. Возвращает длину тела в *out_len.
 * Буфер выделяется с запасом на разворот моно в стерео.
 */
static esp_err_t fetch_wav(const char *body, size_t body_len, uint8_t **out_buf, size_t *out_len)
{
    const char *srv_url = kouki_config_get_srv_url();
    const char *token = kouki_config_get_srv_token();

    char url[256];
    snprintf(url, sizeof(url), "%s/api/speech", srv_url);

    uint8_t *buf = heap_caps_malloc(SPEECH_BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "нет памяти под WAV (%u байт)", (unsigned)SPEECH_BUF_CAP);
        return ESP_ERR_NO_MEM;
    }

    speech_sink_t sink = {.buf = buf, .len = 0, .cap = SPEECH_READ_CAP, .overflow = false};

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .user_data = &sink,
        .event_handler = speech_http_event,
    };

    esp_http_client_handle_t client = NULL;
    esp_err_t err = kouki_http_new_client(&cfg, &client);
    if (err != ESP_OK || client == NULL) {
        free(buf);
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token != NULL && strlen(token) > 0) {
        char auth[300];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, (int)body_len);

    err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    if (status > 0) {
        kouki_ota_mark_server_communicated();
    }

    if (err != ESP_OK) {
        char diag[128] = {0};
        kouki_http_diagnose_error(client, url, err, diag, sizeof(diag));
        ESP_LOGW(TAG, "запрос речи не прошёл: %s (%s)", esp_err_to_name(err), diag);
        esp_http_client_cleanup(client);
        free(buf);
        return err;
    }
    esp_http_client_cleanup(client);

    if (status != 200) {
        /* Тело ошибки короткое ({"error": "..."}) — печатаем его в лог. */
        char text[192] = {0};
        const size_t copy = sink.len < sizeof(text) - 1 ? sink.len : sizeof(text) - 1;
        memcpy(text, sink.buf, copy);
        ESP_LOGW(TAG, "озвучивание недоступно: HTTP %d, %.160s", status, text);
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (sink.overflow || sink.len == 0) {
        ESP_LOGW(TAG, "сервер прислал пустое или слишком большое тело (%u байт%s)",
                 (unsigned)sink.len, sink.overflow ? ", обрезано" : "");
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_buf = buf;
    *out_len = sink.len;
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

static esp_err_t play_single_phrase(const char *phrase, size_t phrase_len)
{
    if (s_abort_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    kouki_voice_state_t voice_state = KOUKI_VOICE_IDLE;
    kouki_voice_get_snapshot(&voice_state, NULL);
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
    const esp_err_t fetch_err = fetch_wav(body, strlen(body), &wav, &wav_len);
    cJSON_free(body);

    if (fetch_err != ESP_OK) {
        ESP_LOGW(TAG, "запрос речи не удался: %s", esp_err_to_name(fetch_err));
        return fetch_err;
    }

    if (s_abort_requested) {
        free(wav);
        return ESP_ERR_INVALID_STATE;
    }

    kouki_voice_get_snapshot(&voice_state, NULL);
    if (voice_state == KOUKI_VOICE_RECORDING || voice_state == KOUKI_VOICE_SENDING) {
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

    if (err == ESP_OK && frames > 0 && !s_abort_requested) {
        const size_t channels = info.channels == 1 ? 2u : (size_t)info.channels;
        const int bytes = (int)(frames * channels * sizeof(int16_t));

        s_busy = true;
        err = esp_audio_play(samples, bytes, portMAX_DELAY);
        s_busy = false;
        kouki_wake_mute(350);
    }

    free(wav);
    return err;
}

esp_err_t kouki_speech_play(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_OK;       /* нечего озвучивать: это не ошибка */
    }

    if (kouki_config_is_silent()) {
        ESP_LOGI(TAG, "режим тишины включен: воспроизведение речи пропущено");
        return ESP_OK;
    }

    if (s_busy || s_is_playing) {
        ESP_LOGW(TAG, "предыдущее озвучивание ещё идёт — пропускаю");
        return ESP_ERR_INVALID_STATE;
    }

    s_is_playing = true;
    s_abort_requested = false;

    ESP_LOGI(TAG, "озвучиваю ответ: %.50s%s", text, strlen(text) > 50 ? "…" : "");

    const char *p = text;
    esp_err_t err = ESP_OK;

    while (*p != '\0' && !s_abort_requested) {
        size_t phrase_len = 0;
        const char *phrase_start = get_next_phrase(p, &phrase_len, 120);
        if (phrase_start == NULL || phrase_len == 0) {
            break;
        }
        p = phrase_start + phrase_len;

        err = play_single_phrase(phrase_start, phrase_len);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "озвучивание прервано или завершилось (%s)", esp_err_to_name(err));
            break;
        }
    }

    if (!s_abort_requested && err == ESP_OK) {
        /* Пауза после речи: закрывает хвост DMA и не даёт повтора */
        esp_audio_play(s_tail, (int)sizeof(s_tail), portMAX_DELAY);
    }

    s_is_playing = false;
    /* Кулдаун 350 мс после речи для рассеивания реверберации комнаты */
    kouki_wake_mute(350);
    return err;
}
#endif // CONFIG_KOUKI_SPEECH_ENABLED
