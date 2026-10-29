#include "app_speech.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"

#include "cJSON.h"
#include "esp_board_init.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "kouki.speech";

#define SPEECH_TIMEOUT_MS 30000
#define SPEECH_MAX_BYTES  (1024 * 1024)   /* минута речи — с запасом */
#define SPEECH_HEADER      44

/* Приём тела ответа: растём в PSRAM, пока не кончится предел или память. */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool overflow;
} speech_sink_t;

static esp_err_t sink_reserve(speech_sink_t *sink, size_t extra)
{
    if (sink->size + extra > SPEECH_MAX_BYTES) {
        sink->overflow = true;
        return ESP_ERR_NO_MEM;
    }

    if (sink->size + extra <= sink->capacity) {
        return ESP_OK;
    }

    size_t capacity = sink->capacity == 0 ? 32 * 1024 : sink->capacity;
    while (capacity < sink->size + extra) {
        capacity *= 2;
    }
    if (capacity > SPEECH_MAX_BYTES) {
        capacity = SPEECH_MAX_BYTES;
    }

    uint8_t *grown = heap_caps_realloc(sink->data, capacity, MALLOC_CAP_SPIRAM);
    if (grown == NULL) {
        return ESP_ERR_NO_MEM;
    }

    sink->data = grown;
    sink->capacity = capacity;
    return ESP_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    speech_sink_t *sink = (speech_sink_t *)event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || sink == NULL || event->data == NULL) {
        return ESP_OK;
    }

    if (sink_reserve(sink, (size_t)event->data_len) != ESP_OK) {
        return ESP_FAIL;
    }

    /* Заголовок может прийти частями — просто дописываем всё подряд и разбираем потом. */
    memcpy(sink->data + sink->size, event->data, (size_t)event->data_len);
    sink->size += (size_t)event->data_len;
    return ESP_OK;
}

/* Описание PCM из WAV: частота и число каналов нужны, чтобы не играть не в том темпе. */
typedef struct {
    size_t offset;      /* смещение данных PCM */
    size_t length;      /* длина данных в байтах */
    uint32_t rate;      /* частота дискретизации */
    uint16_t channels;  /* каналов в файле */
} wav_info_t;

/* Разбирает WAV: ищет fmt и data; -1, если это не WAV. */
static int wav_parse(const uint8_t *bytes, size_t size, wav_info_t *info)
{
    if (size < 12 || memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WAVE", 4) != 0) {
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->channels = 1;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint32_t chunk = (uint32_t)bytes[pos] | ((uint32_t)bytes[pos + 1] << 8)
            | ((uint32_t)bytes[pos + 2] << 16) | ((uint32_t)bytes[pos + 3] << 24);
        const uint32_t len = (uint32_t)bytes[pos + 4] | ((uint32_t)bytes[pos + 5] << 8)
            | ((uint32_t)bytes[pos + 6] << 16) | ((uint32_t)bytes[pos + 7] << 24);

        if (chunk == 0x20746D66u && len >= 16) { /* "fmt " */
            info->channels = (uint16_t)(bytes[pos + 10] | (bytes[pos + 11] << 8));
            info->rate = (uint32_t)bytes[pos + 12] | ((uint32_t)bytes[pos + 13] << 8)
                | ((uint32_t)bytes[pos + 14] << 16) | ((uint32_t)bytes[pos + 15] << 24);
        }

        if (chunk == 0x61746164u) { /* "data" */
            info->offset = pos + 8;
            info->length = (pos + 8 + len <= size) ? len : size - (pos + 8);
            return info->rate == 0 ? -1 : 0;
        }

        pos += 8 + len + (len & 1);
    }

    return -1;
}

/* Линейная передискретизация моно 16/22/24 кГц в 16 кГц — голос не «съезжает» по темпу. */
static size_t resample_to_16k(const int16_t *src, size_t frames, uint32_t rate, int16_t *dst)
{
    if (rate == 16000 || rate == 0) {
        memcpy(dst, src, frames * sizeof(int16_t));
        return frames;
    }

    const size_t out = (size_t)((uint64_t)frames * 16000u / rate);
    for (size_t i = 0; i < out; i++) {
        const size_t pos = (size_t)((uint64_t)i * rate / 16000u);
        dst[i] = src[pos < frames ? pos : frames - 1];
    }
    return out;
}

esp_err_t kouki_speech_play(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "text", text);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/api/speech", CONFIG_KOUKI_SERVER_URL);



    speech_sink_t sink = {0};
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SPEECH_TIMEOUT_MS,
        .event_handler = http_event,
        .user_data = &sink,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    /* Токен в конфиге без префикса: сервер ждёт «Bearer <токен>». */
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_KOUKI_SERVER_TOKEN);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Accept", "audio/wav");

    esp_err_t err = esp_http_client_set_post_field(client, body, (int)strlen(body));
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    free(body);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "озвучивание недоступно: err=%s, HTTP %d, тело %u байт", esp_err_to_name(err), status,
                 (unsigned)sink.size);
        if (sink.data != NULL) {
            heap_caps_free(sink.data);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    wav_info_t info;
    if (wav_parse(sink.data, sink.size, &info) != 0 || info.length < 2) {
        ESP_LOGW(TAG, "ответ не WAV: %u байт", (unsigned)sink.size);
        heap_caps_free(sink.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    /* Сервер отдаёт моно (обычно 22,05 кГц от piper), а BSP открыт на 16 кГц стерео. */
    const size_t frames = info.length / 2 / (info.channels == 0 ? 1 : info.channels);
    int16_t *mono = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (mono == NULL) {
        heap_caps_free(sink.data);
        return ESP_ERR_NO_MEM;
    }

    const int16_t *raw = (const int16_t *)(sink.data + info.offset);
    if (info.channels == 1) {
        memcpy(mono, raw, frames * sizeof(int16_t));
    } else {
        for (size_t i = 0; i < frames; i++) {
            mono[i] = raw[i * info.channels];      /* берём левый канал */
        }
    }

    const size_t resampled = resample_to_16k(mono, frames, info.rate, mono);
    int16_t *stereo = heap_caps_malloc(resampled * 4, MALLOC_CAP_SPIRAM);
    if (stereo == NULL) {
        heap_caps_free(mono);
        heap_caps_free(sink.data);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < resampled; i++) {
        stereo[i * 2] = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }

    ESP_LOGI(TAG, "озвучиваю ответ: %.60s… (%u кадров, было %lu Гц / %u кан.)", text, (unsigned)resampled,
             (unsigned long)info.rate, (unsigned)info.channels);
    const esp_err_t played = esp_audio_play(stereo, (int)(resampled * 4), portMAX_DELAY);
    ESP_LOGI(TAG, "воспроизведение: %s", esp_err_to_name(played));

    heap_caps_free(stereo);
    heap_caps_free(mono);
    heap_caps_free(sink.data);
    return played;
}
