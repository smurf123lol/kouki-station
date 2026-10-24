#include "app_voice.h"

#include <string.h>

#include "cJSON.h"
#include "esp_board_init.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "kouki.voice";

#define SAMPLE_RATE      16000
#define BYTES_PER_SAMPLE 2
#define CHANNELS         2          /* столько просим у BSP; реально приходит 4 */
#define WAV_HEADER_SIZE  44
#define RECORD_CHUNK     1024       /* кадров за одно чтение I2S */

static SemaphoreHandle_t s_lock;
static kouki_voice_state_t s_state = KOUKI_VOICE_IDLE;
static kouki_voice_result_t s_result;
static kouki_session_t s_session = KOUKI_SESSION_TASK;

static kouki_voice_event_cb_t s_cb;
static void *s_cb_ctx;

static uint8_t *s_buf;              /* WAV целиком: заголовок + сэмплы */
static size_t s_capacity_bytes;     /* максимум сэмплов под CONFIG_KOUKI_MAX_RECORD_S */
static size_t s_sample_bytes;       /* сколько уже записано (без заголовка) */
static volatile bool s_recording;

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* WAV, формат PCM16 моно 16 кГц — ровно то, что принимает /api/voice. */
static void wav_write_header(uint8_t *buf, uint32_t sample_bytes)
{
    memcpy(buf + 0, "RIFF", 4);
    put_le32(buf + 4, 36 + sample_bytes);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_le32(buf + 16, 16);
    put_le16(buf + 20, 1);                      /* формат 1 = PCM */
    put_le16(buf + 22, 1);                      /* каналов */
    put_le32(buf + 24, SAMPLE_RATE);
    put_le32(buf + 28, SAMPLE_RATE * BYTES_PER_SAMPLE);
    put_le16(buf + 32, BYTES_PER_SAMPLE);
    put_le16(buf + 34, 16);                     /* бит на сэмпл */
    memcpy(buf + 36, "data", 4);
    put_le32(buf + 40, sample_bytes);
}

static void publish(kouki_voice_state_t state, const kouki_voice_result_t *result)
{
    kouki_voice_result_t copy = {0};
    if (result) {
        copy = *result;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_state = state;
        if (result) {
            s_result = copy;
        }
        xSemaphoreGive(s_lock);
    }

    if (s_cb) {
        s_cb(state, result ? &copy : NULL, s_cb_ctx);
    }
}

/* Читает один кусок с микрофона и складывает выбранный канал в s_buf. */
static esp_err_t record_chunk(void)
{
    const int channels = esp_get_feed_channel() > 0 ? esp_get_feed_channel() : 4;
    const int mic = CONFIG_KOUKI_MIC_CHANNEL < channels ? CONFIG_KOUKI_MIC_CHANNEL : 0;
    static int16_t raw[RECORD_CHUNK * 4];

    if (mic >= 4) {
        return ESP_ERR_INVALID_STATE;
    }

    const int bytes = RECORD_CHUNK * channels * BYTES_PER_SAMPLE;
    ESP_RETURN_ON_ERROR(esp_get_feed_data(true, raw, bytes), TAG, "чтение микрофона не удалось");

    for (int i = 0; i < RECORD_CHUNK; i++) {
        if (s_sample_bytes + BYTES_PER_SAMPLE > s_capacity_bytes) {
            return ESP_ERR_NO_MEM;
        }
        const int16_t sample = raw[i * channels + mic];
        memcpy(s_buf + WAV_HEADER_SIZE + s_sample_bytes, &sample, BYTES_PER_SAMPLE);
        s_sample_bytes += BYTES_PER_SAMPLE;
    }

    return ESP_OK;
}

static void record_task(void *arg)
{
    (void)arg;
    while (true) {
        if (!s_recording) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (record_chunk() != ESP_OK) {
            /* Кончилось место — закрываем запись так же, как по кнопке. */
            s_recording = false;
            kouki_voice_stop_and_send();
        }
    }
}

static esp_err_t http_post_wav(const uint8_t *wav, size_t wav_len, char *response, size_t response_len, int *status_out)
{
    char url[192];

    /*
     * Сессия задаёт смысл записи, а не сервер угадывает его по словам:
     * «Записать» -> intent=add_task (фраза становится задачей целиком),
     * «Спросить» -> intent не передаётся, фразу разбирает классификатор.
     */
    if (s_session == KOUKI_SESSION_TASK) {
        snprintf(url, sizeof(url), "%s/api/voice?source=%s&intent=add_task",
                 CONFIG_KOUKI_SERVER_URL, CONFIG_KOUKI_SOURCE);
    } else {
        snprintf(url, sizeof(url), "%s/api/voice?source=%s",
                 CONFIG_KOUKI_SERVER_URL, CONFIG_KOUKI_SOURCE);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .buffer_size = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    if (strlen(CONFIG_KOUKI_SERVER_TOKEN) > 0) {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_KOUKI_SERVER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t err = esp_http_client_open(client, (int)wav_len);
    if (err == ESP_OK) {
        int written = esp_http_client_write(client, (const char *)wav, (int)wav_len);
        if (written != (int)wav_len) {
            ESP_LOGE(TAG, "отправлено %d из %u байт", written, (unsigned)wav_len);
            err = ESP_FAIL;
        }
    }

    int status = 0;
    if (err == ESP_OK) {
        err = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
    }

    size_t total = 0;
    if (err == ESP_OK) {
        while (total + 1 < response_len) {
            const int read = esp_http_client_read(client, response + total, (int)(response_len - 1 - total));
            if (read <= 0) {
                break;
            }
            total += (size_t)read;
        }
        response[total] = '\0';
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status_out) {
        *status_out = status;
    }
    if (err != ESP_OK) {
        return err;
    }
    return status == 200 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst_len) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

/* Разбирает ответ /api/voice — только те поля, которые показывает экран. */
static void parse_response(const char *json, kouki_voice_result_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        copy_str(out->error, sizeof(out->error), "Сервер ответил не-JSON");
        return;
    }

    const cJSON *intake = cJSON_GetObjectItemCaseSensitive(root, "intake");
    const char *err = json_string(root, "error");

    copy_str(out->transcript, sizeof(out->transcript), json_string(root, "transcript"));
    out->has_transcript = out->transcript[0] != '\0';

    if (err) {
        copy_str(out->error, sizeof(out->error), err);
    }

    if (cJSON_IsObject(intake)) {
        const cJSON *intent = cJSON_GetObjectItemCaseSensitive(intake, "intent");
        if (cJSON_IsObject(intent)) {
            copy_str(out->intent, sizeof(out->intent), json_string(intent, "intent"));
        }
        copy_str(out->outcome, sizeof(out->outcome), json_string(intake, "outcome"));
        copy_str(out->command, sizeof(out->command), json_string(intake, "command"));

        const cJSON *task = cJSON_GetObjectItemCaseSensitive(intake, "task");
        if (cJSON_IsObject(task)) {
            copy_str(out->task_title, sizeof(out->task_title), json_string(task, "title"));
            copy_str(out->task_status, sizeof(out->task_status), json_string(task, "status"));
        }
        const char *intake_error = json_string(intake, "error");
        if (intake_error && out->error[0] == '\0') {
            copy_str(out->error, sizeof(out->error), intake_error);
        }
    }

    cJSON_Delete(root);
}

static void send_task(void *arg)
{
    (void)arg;
    kouki_voice_result_t result = {0};

    const uint32_t frames = (uint32_t)(s_sample_bytes / BYTES_PER_SAMPLE);
    result.duration_ms = (uint32_t)((uint64_t)frames * 1000 / SAMPLE_RATE);
    wav_write_header(s_buf, (uint32_t)s_sample_bytes);

    char response[4096] = {0};
    publish(KOUKI_VOICE_SENDING, &result);

    /* Тик и HTTP живут в одной задаче; сторожевой таймер не трогаем. */
    const esp_err_t err = http_post_wav(s_buf, WAV_HEADER_SIZE + s_sample_bytes, response, sizeof(response),
                                       &result.http_status);
    if (err != ESP_OK && response[0] == '\0') {
        copy_str(result.error, sizeof(result.error),
                 err == ESP_ERR_INVALID_RESPONSE ? "Сервер ответил ошибкой" : "Сервер недоступен");
        publish(KOUKI_VOICE_ERROR, &result);
        vTaskDelete(NULL);
        return;
    }

    parse_response(response, &result);
    if (result.error[0] == '\0' && err == ESP_ERR_INVALID_RESPONSE) {
        copy_str(result.error, sizeof(result.error), "Сервер ответил ошибкой");
    }
    if (result.error[0] != '\0') {
        publish(KOUKI_VOICE_ERROR, &result);
    } else {
        publish(KOUKI_VOICE_DONE, &result);
    }

    vTaskDelete(NULL);
}

esp_err_t kouki_voice_init(kouki_voice_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_capacity_bytes = (size_t)CONFIG_KOUKI_MAX_RECORD_S * SAMPLE_RATE * BYTES_PER_SAMPLE;
    s_buf = heap_caps_malloc(WAV_HEADER_SIZE + s_capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGE(TAG, "не хватило PSRAM под %u байт записи", (unsigned)(WAV_HEADER_SIZE + s_capacity_bytes));
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(record_task, "kouki.record", 4096, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "запись до %d с, канал %d, сервер %s", CONFIG_KOUKI_MAX_RECORD_S,
             CONFIG_KOUKI_MIC_CHANNEL, CONFIG_KOUKI_SERVER_URL);
    return ESP_OK;
}

esp_err_t kouki_voice_set_session(kouki_session_t session)
{
    if (s_state == KOUKI_VOICE_RECORDING || s_state == KOUKI_VOICE_SENDING) {
        /* Сессию меняют до записи: иначе непонятно, что отправится. */
        return ESP_ERR_INVALID_STATE;
    }

    s_session = session;
    ESP_LOGI(TAG, "сессия: %s", session == KOUKI_SESSION_TASK ? "запись" : "вопрос");
    return ESP_OK;
}

esp_err_t kouki_voice_start_recording(void)
{
    if (s_state == KOUKI_VOICE_RECORDING) {
        return ESP_OK;
    }

    s_sample_bytes = 0;
    s_recording = true;
    publish(KOUKI_VOICE_RECORDING, NULL);
    return ESP_OK;
}

/// <summary>
/// Проверяет, есть ли в записи голос. На тишине распознаватель выдаёт галлюцинации
/// вроде «(звук пилот)», и они попадают в задачи, поэтому слишком тихое не отправляем.
/// </summary>
static bool IsLoudEnough(void)
{
    const size_t samples = s_sample_bytes / BYTES_PER_SAMPLE;
    if (samples == 0) {
        return false;
    }

    const int16_t *pcm = (const int16_t *)(s_buf + WAV_HEADER_SIZE);
    uint64_t sum = 0;
    size_t checked = 0;

    // Каждый четвёртый сэмпл: на 15 секундах это 60 000 проверок — для оценки хватает.
    for (size_t i = 0; i < samples; i += 4) {
        int32_t value = pcm[i];
        sum += (uint64_t)(value < 0 ? -value : value);
        checked++;
    }

    const uint64_t mean = checked ? sum / checked : 0;
    ESP_LOGI(TAG, "средняя амплитуда записи: %u", (unsigned)mean);
    return mean >= CONFIG_KOUKI_MIN_AMPLITUDE;
}

esp_err_t kouki_voice_stop_and_send(void)
{
    if (s_state != KOUKI_VOICE_RECORDING) {
        return ESP_OK;
    }

    s_recording = false;
    if (s_sample_bytes == 0 || !IsLoudEnough()) {
        kouki_voice_result_t result = {0};
        copy_str(result.error, sizeof(result.error),
                  s_sample_bytes == 0
                      ? "Тишина: ничего не записалось"
                      : "Тишина: слишком тихо, повторите ближе к микрофону");
        publish(KOUKI_VOICE_ERROR, &result);
        return ESP_OK;
    }

    if (xTaskCreate(send_task, "kouki.send", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool kouki_voice_is_recording(void)
{
    return s_state == KOUKI_VOICE_RECORDING;
}

void kouki_voice_get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result)
{
    if (!s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (state) {
            *state = s_state;
        }
        if (result) {
            *result = s_result;
        }
        xSemaphoreGive(s_lock);
    }
}
