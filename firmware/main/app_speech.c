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

#else

#include "cJSON.h"
#include "app_config.h"
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

bool kouki_speech_busy(void)
{
    return s_busy;
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
static size_t utf8_chars(const char *s)
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
static char *utf8_prefix_copy(const char *s, size_t max_chars)
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
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token != NULL && strlen(token) > 0) {
        char auth[300];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, body, (int)body_len);

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "запрос речи не прошёл: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }

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

esp_err_t kouki_speech_play(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_OK;       /* нечего озвучивать: это не ошибка */
    }

    if (s_busy) {
        ESP_LOGW(TAG, "предыдущее озвучивание ещё идёт — пропускаю");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "озвучиваю ответ: %.40s%s", text, strlen(text) > 40 ? "…" : "");

    /*
     * Лимит — в символах (как MaxAnswerChars на сервере), а не байтах: иначе
     * кириллица обрезается вдвое раньше, а порез посреди символа даёт 400.
     */
    const size_t chars = utf8_chars(text);
    char *trimmed = utf8_prefix_copy(text, KOUKI_SPEECH_MAX_CHARS);
    if (trimmed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (chars > KOUKI_SPEECH_MAX_CHARS) {
        ESP_LOGW(TAG, "ответ длиннее %u символов — озвучу начало",
                 (unsigned)KOUKI_SPEECH_MAX_CHARS);
    }

    char *body = build_body(trimmed);
    free(trimmed);

    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    /* Во время сетевого запроса s_busy НЕ взводится: кнопка записи остаётся рабочей */
    const esp_err_t fetch_err = fetch_wav(body, strlen(body), &wav, &wav_len);
    cJSON_free(body);

    esp_err_t err = fetch_err;
    if (err == ESP_OK) {
        /* Проверяем, не началась ли новая запись/отправка, пока шёл HTTP-запрос */
        kouki_voice_state_t voice_state = KOUKI_VOICE_IDLE;
        kouki_voice_get_snapshot(&voice_state, NULL);
        if (voice_state == KOUKI_VOICE_RECORDING || voice_state == KOUKI_VOICE_SENDING) {
            ESP_LOGI(TAG, "во время запроса речи началась запись/отправка — воспроизведение отменено");
            free(wav);
            return ESP_OK;
        }

        kouki_speech_wav_t info;
        err = kouki_speech_wav_parse(wav, wav_len, &info);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "сервер прислал не тот WAV (err=%s, %u байт)", esp_err_to_name(err),
                     (unsigned)wav_len);
        } else if (info.sample_rate != 16000) {
            /* Кодек открыт на 16 кГц: другой частоте звук будет не тот. */
            ESP_LOGW(TAG, "неожиданная частота WAV: %u Гц", (unsigned)info.sample_rate);
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            int16_t *samples = (int16_t *)(wav + info.data_offset);
            const size_t frames = info.data_bytes / (sizeof(int16_t) * info.channels);

            if (frames > 0 && info.channels == 1) {
                err = kouki_speech_stereo_expand(samples, frames);
            }

            if (err == ESP_OK && frames > 0) {
                const size_t channels = info.channels == 1 ? 2u : (size_t)info.channels;
                const int bytes = (int)(frames * channels * sizeof(int16_t));

                ESP_LOGI(TAG, "озвучиваю %u кадров (%.1f с)", (unsigned)frames,
                         (double)frames / 16000.0);
                /* s_busy взводится только на время фактического вывода в кодек */
                s_busy = true;
                err = esp_audio_play(samples, bytes, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "кодек не принял речь: %s", esp_err_to_name(err));
                } else {
                    /* Пауза после речи: закрывает хвост DMA и не даёт повтора. */
                    esp_audio_play(s_tail, (int)sizeof(s_tail), portMAX_DELAY);
                }
                s_busy = false;
            }
        }
    } else {
        ESP_LOGW(TAG, "озвучивание не удалось: %s", esp_err_to_name(err));
    }

    free(wav);
    return err;
}
#endif // CONFIG_KOUKI_SPEECH_ENABLED
