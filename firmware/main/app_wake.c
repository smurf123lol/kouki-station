#include "app_wake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

// esp-sr: интерфейс wake-word и загрузка моделей из flash.
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "kouki.wake";

static srmodel_list_t *s_models;
static const esp_wn_iface_t *s_iface;
static model_iface_data_t *s_data;
static int16_t *s_chunk_buf;     // ровно один чанк детектора
static int s_chunk_samples;      // сколько ждём
static int s_filled;             // сколько уже набрали
static char s_model[64];

esp_err_t kouki_wake_init(void)
{
    s_models = esp_srmodel_init(CONFIG_KOUKI_WAKE_PARTITION);
    if (!s_models) {
        // esp-sr уже написал причину; здесь важно не падать — станция живёт без слова.
        ESP_LOGE(TAG, "нет модели в партиции '%s': проверьте partitions.csv и CONFIG_SR_WN_*",
                 CONFIG_KOUKI_WAKE_PARTITION);
        return ESP_ERR_NOT_FOUND;
    }

    char *name = NULL;
    if (strlen(CONFIG_KOUKI_WAKE_MODEL) > 0) {
        name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, CONFIG_KOUKI_WAKE_MODEL);
    }
    if (!name) {
        name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    }
    if (!name) {
        ESP_LOGE(TAG, "модели wake-word в партиции нет: включите CONFIG_SR_WN_WN10_HIESP");
        return ESP_ERR_NOT_FOUND;
    }

    s_iface = esp_wn_handle_from_name(name);
    if (!s_iface) {
        ESP_LOGE(TAG, "esp-sr не знает модель '%s'", name);
        return ESP_ERR_NOT_FOUND;
    }

    // 90 — штатный режим: модель читается из flash, а не поднимается в PSRAM целиком.
    s_data = s_iface->create(name, DET_MODE_90);
    if (!s_data) {
        ESP_LOGE(TAG, "детектор не создался (мало памяти?)");
        return ESP_ERR_NO_MEM;
    }

    s_chunk_samples = s_iface->get_samp_chunksize(s_data);
    const int rate = s_iface->get_samp_rate(s_data);
    s_chunk_buf = heap_caps_malloc((size_t)s_chunk_samples * sizeof(int16_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_chunk_buf) {
        ESP_LOGE(TAG, "не хватило внутренней памяти под чанк %d сэмплов", s_chunk_samples);
        s_iface->destroy(s_data);
        s_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_filled = 0;

    // Порог задаётся на слово (нумерация в esp-sr с единицы).
    const float threshold = (float)CONFIG_KOUKI_WAKE_THRESHOLD / 100.0f;
    const int words = s_iface->get_word_num(s_data);
    for (int i = 1; i <= words; i++) {
        s_iface->set_det_threshold(s_data, threshold, i);
    }

    snprintf(s_model, sizeof(s_model), "%s", name);
    char *word = esp_wn_wakeword_from_name(name);
    ESP_LOGI(TAG, "детектор готов: модель %s, слово «%s», порог %.2f, чанк %d сэмплов, %d Гц",
             s_model, word ? word : "?", (double)threshold, s_chunk_samples, rate);

    if (rate != 16000) {
        // Микрофон отдаёт 16 кГц; детектор на другой частоте будет слышать «не то».
        ESP_LOGW(TAG, "детектор ждёт %d Гц, а микрофон отдаёт 16000 — слово может не срабатывать", rate);
    }

    return ESP_OK;
}

void kouki_wake_deinit(void)
{
    if (s_data && s_iface) {
        s_iface->destroy(s_data);
    }
    s_data = NULL;
    s_iface = NULL;
    free(s_chunk_buf);
    s_chunk_buf = NULL;
    s_chunk_samples = 0;
    s_filled = 0;
    if (s_models) {
        esp_srmodel_deinit(s_models);
    }
    s_models = NULL;
    s_model[0] = '\0';
}

bool kouki_wake_is_ready(void)
{
    return s_data != NULL;
}

const char *kouki_wake_model(void)
{
    return s_model[0] ? s_model : "none";
}

bool kouki_wake_feed(const int16_t *pcm, int samples)
{
    if (!s_data || !pcm || samples <= 0) {
        return false;
    }

    bool detected = false;

    for (int i = 0; i < samples; i++) {
        s_chunk_buf[s_filled++] = pcm[i];
        if (s_filled < s_chunk_samples) {
            continue;
        }

        if (s_iface->detect(s_data, s_chunk_buf) == WAKENET_DETECTED) {
            detected = true;
        }
        s_filled = 0;
    }

    return detected;
}
