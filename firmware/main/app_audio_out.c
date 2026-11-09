#include "app_audio_out.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static SemaphoreHandle_t s_audio_out_mutex = NULL;
static portMUX_TYPE s_audio_out_spinlock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t kouki_audio_out_init(void)
{
    if (s_audio_out_mutex != NULL) {
        return ESP_OK;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_audio_out_spinlock);
    if (s_audio_out_mutex == NULL) {
        s_audio_out_mutex = created;
        created = NULL;
    }
    taskEXIT_CRITICAL(&s_audio_out_spinlock);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }
    return ESP_OK;
}

void kouki_audio_out_lock(void)
{
    if (s_audio_out_mutex == NULL) {
        (void)kouki_audio_out_init();
    }
    if (s_audio_out_mutex != NULL) {
        xSemaphoreTake(s_audio_out_mutex, portMAX_DELAY);
    }
}

void kouki_audio_out_unlock(void)
{
    if (s_audio_out_mutex != NULL) {
        xSemaphoreGive(s_audio_out_mutex);
    }
}

#else

esp_err_t kouki_audio_out_init(void)
{
    return ESP_OK;
}

void kouki_audio_out_lock(void)
{
}

void kouki_audio_out_unlock(void)
{
}

#endif
