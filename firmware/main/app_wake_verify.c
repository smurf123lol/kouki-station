#include "app_wake_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_http.h"

static const char *TAG = "kouki.wake.verify";

typedef struct {
    uint8_t *wav;
    size_t wav_len;
} verify_job_t;

static volatile int s_state = KOUKI_WAKE_VERIFY_IDLE;
static TaskHandle_t s_task = NULL;
static char s_transcript[160];

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_wav_header(uint8_t *buf, size_t pcm_bytes)
{
    memcpy(buf + 0, "RIFF", 4);
    put_le32(buf + 4, (uint32_t)(36 + pcm_bytes));
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_le32(buf + 16, 16);
    put_le16(buf + 20, 1);      /* PCM */
    put_le16(buf + 22, 1);      /* моно */
    put_le32(buf + 24, 16000);  /* частота */
    put_le32(buf + 28, 16000 * 2);
    put_le16(buf + 32, 2);      /* выравнивание */
    put_le16(buf + 34, 16);     /* бит на сэмпл */
    memcpy(buf + 36, "data", 4);
    put_le32(buf + 40, (uint32_t)pcm_bytes);
}

static bool contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

bool kouki_wake_verify_matches(const char *transcript)
{
    if (!transcript || transcript[0] == '\0') {
        return false;
    }

    /* Whisper на живой записи почти никогда не пишет «эй гин» дословно:
       в замерах встречались «Эй, Дин!», «Эдит!», «Пейдем!», «Рейдин, покой…».
       Поэтому принимаем, если в тексте есть характерное слово семейства
       «гин/дин/жин», либо текст начинается с короткого обращения. */
    const bool has_word = contains(transcript, "гин") || contains(transcript, "гын") ||
                          contains(transcript, "гим") || contains(transcript, "жин") ||
                          contains(transcript, "джин") || contains(transcript, "дин") ||
                          contains(transcript, "дим") || contains(transcript, "ден") ||
                          contains(transcript, "дем") || contains(transcript, "дан");
    if (has_word) {
        return true;
    }

    const size_t len = strlen(transcript);
    if (len > 16) {
        return false;
    }

    const bool has_prefix = contains(transcript, "эй") || contains(transcript, "ей") ||
                            contains(transcript, "ай") || contains(transcript, "шей") ||
                            contains(transcript, "рей") || contains(transcript, "бей") ||
                            contains(transcript, "гей") || contains(transcript, "пей") ||
                            contains(transcript, "дей") || contains(transcript, "эд") ||
                            contains(transcript, "ед");
    return has_prefix;
}

/* Достаёт значение поля "transcript" из JSON без полноценного парсера:
   для двух полей этого достаточно, а зависимость на cJSON в этой задаче не нужна. */
static bool extract_transcript(const char *json, char *out, size_t out_len)
{
    if (!json || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *key = strstr(json, "\"transcript\"");
    if (!key) {
        return false;
    }
    const char *colon = strchr(key, ':');
    if (!colon) {
        return false;
    }
    const char *q = strchr(colon, '"');
    if (!q) {
        return false;
    }
    ++q;

    size_t written = 0;
    while (*q && *q != '"' && written + 1 < out_len) {
        if (*q == '\\' && (q[1] == '"' || q[1] == '\\')) {
            out[written++] = q[1];
            q += 2;
            continue;
        }
        /* \uXXXX и прочие escape-последовательности не разворачиваем:
           для сверки хватит и «сырых» байтов. */
        out[written++] = *q++;
    }
    out[written] = '\0';
    return true;
}

static void verify_task(void *arg)
{
    verify_job_t *job = (verify_job_t *)arg;
    int result = KOUKI_WAKE_VERIFY_UNAVAILABLE;
    s_transcript[0] = '\0';

    if (job && job->wav && job->wav_len > 44) {
        const char *srv_url = kouki_config_get_srv_url();
        const char *srv_token = kouki_config_get_srv_token();

        char url[300];
        snprintf(url, sizeof(url), "%s/api/transcribe", srv_url ? srv_url : "");

        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = HTTP_METHOD_POST;
        cfg.timeout_ms = CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS;
        cfg.buffer_size = 4096;

        esp_http_client_handle_t client = NULL;
        esp_err_t err = kouki_http_new_client(&cfg, &client);
        if (err == ESP_OK && client) {
            esp_http_client_set_header(client, "Content-Type", "audio/wav");
            if (srv_token && srv_token[0] != '\0') {
                char auth[300];
                snprintf(auth, sizeof(auth), "Bearer %s", srv_token);
                esp_http_client_set_header(client, "Authorization", auth);
            }

            err = esp_http_client_open(client, (int)job->wav_len);
            if (err == ESP_OK) {
                size_t sent = 0;
                while (sent < job->wav_len) {
                    size_t left = job->wav_len - sent;
                    int chunk = (left < 4096u) ? (int)left : 4096;
                    int w = esp_http_client_write(client, (const char *)(job->wav + sent), chunk);
                    if (w <= 0) {
                        err = ESP_FAIL;
                        break;
                    }
                    sent += (size_t)w;
                }
            }

            if (err == ESP_OK) {
                int64_t hdr = esp_http_client_fetch_headers(client);
                const int status = esp_http_client_get_status_code(client);
                char body[512];
                size_t total = 0;
                body[0] = '\0';

                if (hdr >= 0) {
                    while (total + 1 < sizeof(body)) {
                        int r = esp_http_client_read(client, body + total, (int)(sizeof(body) - 1 - total));
                        if (r <= 0) {
                            break;
                        }
                        total += (size_t)r;
                    }
                    body[total] = '\0';
                }

                if (status >= 200 && status < 300) {
                    if (extract_transcript(body, s_transcript, sizeof(s_transcript))) {
                        result = kouki_wake_verify_matches(s_transcript)
                                     ? KOUKI_WAKE_VERIFY_ACCEPTED
                                     : KOUKI_WAKE_VERIFY_REJECTED;
                        ESP_LOGI(TAG, "ASR-подтверждение: «%s» -> %s", s_transcript,
                                 result == KOUKI_WAKE_VERIFY_ACCEPTED ? "принято" : "отклонено");
                    } else {
                        ESP_LOGW(TAG, "в ответе нет transcript: %s", body);
                    }
                } else if (status == 422) {
                    result = KOUKI_WAKE_VERIFY_REJECTED;
                    ESP_LOGI(TAG, "ASR не распознал речь — хит отклонён");
                } else {
                    ESP_LOGW(TAG, "ASR-проверка: HTTP %d", status);
                }
            } else {
                char diag[96] = {0};
                kouki_http_diagnose_error(client, url, err, diag, sizeof(diag));
                ESP_LOGW(TAG, "ASR-проверка недоступна: %s", diag);
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        } else {
            ESP_LOGW(TAG, "не удалось создать HTTP-клиент для ASR-проверки");
            if (client) {
                esp_http_client_cleanup(client);
            }
        }
    }

    if (job) {
        free(job->wav);
        free(job);
    }

    s_state = result;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t kouki_wake_verify_start(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t pcm_bytes = (size_t)samples * 2u;
    verify_job_t *job = (verify_job_t *)calloc(1, sizeof(verify_job_t));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *wav = (uint8_t *)heap_caps_malloc(44 + pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = (uint8_t *)malloc(44 + pcm_bytes);
    }
    if (!wav) {
        free(job);
        return ESP_ERR_NO_MEM;
    }

    write_wav_header(wav, pcm_bytes);
    memcpy(wav + 44, pcm, pcm_bytes);

    job->wav = wav;
    job->wav_len = 44 + pcm_bytes;

    s_state = KOUKI_WAKE_VERIFY_PENDING;
    s_transcript[0] = '\0';

    if (xTaskCreate(verify_task, "wake_verify", 8192, job, tskIDLE_PRIORITY + 1, &s_task) != pdPASS) {
        s_task = NULL;
        s_state = KOUKI_WAKE_VERIFY_UNAVAILABLE;
        free(wav);
        free(job);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

kouki_wake_verify_state_t kouki_wake_verify_state(void)
{
    return (kouki_wake_verify_state_t)s_state;
}

void kouki_wake_verify_reset(void)
{
    /* Задачу не убиваем: она сама выставит результат и завершится. */
    if (s_task == NULL) {
        s_state = KOUKI_WAKE_VERIFY_IDLE;
        s_transcript[0] = '\0';
    }
}

const char *kouki_wake_verify_last_transcript(void)
{
    return s_transcript;
}
