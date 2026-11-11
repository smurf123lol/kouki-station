/*
 * Окружение host-стенда Wake (T121 П9): семафор, задача, FFT, heap_caps, время.
 *
 * Зачем это всё нужно:
 * - app_wake.cpp берёт leaf-мьютекс состояния лениво, под критической секцией;
 * - app_wake_verify.cpp создаёт задачу на POST /api/transcribe; в стенде
 *   задача не запускается сама, тест прогоняет её в текущем потоке;
 * - esp-dl dl_rfft заменён честным DFT с тем же форматом выхода, чтобы
 *   mel-спектр и инференс модели считались по-настоящему (граница
 *   доказательства: значения могут отличаться от esp-dl на погрешности float);
 * - heap_caps_malloc на хосте — malloc (PSRAM нет);
 * - время — переменная, которую двигает тест (проверка mute).
 *
 * Сборка: gcc -std=gnu11, линковка с C++-модулями через extern "C" в заголовках.
 */
#include "wake_mock.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Мьютекс состояния -------------------------------------------------- */

struct wake_sem_mock {
    int taken; /* 0 — свободен, 1 — взят: повторный захват в стенде — ошибка */
    int level;
};

static struct wake_sem_mock s_sem;
static int s_sem_creates = 0;
static int s_sem_takes = 0;
static int s_sem_gives = 0;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    s_sem_creates++;
    s_sem.taken = 0;
    s_sem.level = 0;
    return &s_sem;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout)
{
    (void)timeout;
    if (sem == NULL) {
        return pdFALSE;
    }
    if (sem->taken != 0) {
        /* Рекурсивный захват leaf-мьютекса — дефект модуля, а не стенда. */
        fprintf(stderr, "wake_mock: повторный захват мьютекса состояния\n");
        return pdFALSE;
    }
    sem->taken = 1;
    sem->level = 1;
    s_sem_takes++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (sem == NULL || sem->taken == 0) {
        fprintf(stderr, "wake_mock: освобождение свободного мьютекса\n");
        return pdFALSE;
    }
    sem->taken = 0;
    sem->level = 0;
    s_sem_gives++;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    (void)sem;
}

int wake_mock_semaphore_takes(void)
{
    return s_sem_takes;
}

int wake_mock_semaphore_creates(void)
{
    return s_sem_creates;
}

int wake_mock_semaphore_gives(void)
{
    return s_sem_gives;
}

bool wake_mock_semaphore_held(void)
{
    return s_sem.taken != 0;
}

/* --- Задача ASR-проверки ------------------------------------------------ */

struct wake_task_mock {
    void (*fn)(void *);
    void *arg;
    char name[24];
    uint32_t stack;
    unsigned priority;
};

static struct wake_task_mock s_task;
static TaskHandle_t s_task_handle = NULL;
static bool s_fail_next_task_create = false;
static int s_task_creates = 0;

BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack,
                       void *arg, unsigned priority, TaskHandle_t *out)
{
    if (s_fail_next_task_create) {
        s_fail_next_task_create = false;
        if (out != NULL) {
            *out = NULL;
        }
        return pdFALSE;
    }
    s_task.fn = task;
    s_task.arg = arg;
    snprintf(s_task.name, sizeof(s_task.name), "%s", name ? name : "");
    s_task.stack = stack;
    s_task.priority = priority;
    s_task_handle = &s_task;
    s_task_creates++;
    if (out != NULL) {
        *out = s_task_handle;
    }
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
    s_task_handle = NULL;
}

void wake_mock_fail_next_task_create(void)
{
    s_fail_next_task_create = true;
}

int wake_mock_task_creates(void)
{
    return s_task_creates;
}

bool wake_mock_run_task(void)
{
    if (s_task_handle == NULL || s_task.fn == NULL) {
        return false;
    }
    void (*fn)(void *) = s_task.fn;
    void *arg = s_task.arg;
    s_task.fn = NULL;
    s_task.arg = NULL;
    fn(arg);
    return true;
}

uint32_t wake_mock_last_task_stack(void)
{
    return s_task.stack;
}

unsigned wake_mock_last_task_priority(void)
{
    return s_task.priority;
}

const char *wake_mock_last_task_name(void)
{
    return s_task.name;
}

/* --- FFT (наивный DFT с форматом выхода esp-dl) ------------------------- */

#define WAKE_MOCK_MAX_FFT 512

struct dl_fft_f32_t {
    int n;
    float cos_tab[WAKE_MOCK_MAX_FFT];
    float sin_tab[WAKE_MOCK_MAX_FFT];
};

static bool s_fail_next_rfft_init = false;
static int s_rfft_inits = 0;
static int s_rfft_runs = 0;

dl_fft_f32_t *dl_rfft_f32_init(int fft_point, uint32_t caps)
{
    (void)caps;
    if (s_fail_next_rfft_init) {
        s_fail_next_rfft_init = false;
        return NULL;
    }
    if (fft_point <= 0 || fft_point > WAKE_MOCK_MAX_FFT) {
        return NULL;
    }
    dl_fft_f32_t *h = (dl_fft_f32_t *)calloc(1, sizeof(dl_fft_f32_t));
    if (h == NULL) {
        return NULL;
    }
    h->n = fft_point;
    for (int j = 0; j < fft_point; j++) {
        const double angle = 2.0 * M_PI * (double)j / (double)fft_point;
        h->cos_tab[j] = (float)cos(angle);
        h->sin_tab[j] = (float)sin(angle);
    }
    s_rfft_inits++;
    return h;
}

void dl_rfft_f32_deinit(dl_fft_f32_t *handle)
{
    free(handle);
}

esp_err_t dl_rfft_f32_run(dl_fft_f32_t *handle, float *data)
{
    if (handle == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int n = handle->n;
    float in[WAKE_MOCK_MAX_FFT];
    memcpy(in, data, (size_t)n * sizeof(float));

    for (int k = 0; k <= n / 2; k++) {
        float re = 0.0f;
        float im = 0.0f;
        for (int t = 0; t < n; t++) {
            const int idx = (k * t) % n;
            re += in[t] * handle->cos_tab[idx];
            im -= in[t] * handle->sin_tab[idx];
        }
        if (k == 0) {
            data[0] = re;
        } else if (k == n / 2) {
            data[1] = re;
        } else {
            data[2 * k] = re;
            data[2 * k + 1] = im;
        }
    }
    s_rfft_runs++;
    return ESP_OK;
}

void wake_mock_fail_next_rfft_init(void)
{
    s_fail_next_rfft_init = true;
}

int wake_mock_rfft_inits(void)
{
    return s_rfft_inits;
}

int wake_mock_rfft_runs(void)
{
    return s_rfft_runs;
}

/* --- heap_caps ---------------------------------------------------------- */

static int s_heap_calls = 0;
static int s_heap_spiram_calls = 0;
static int s_heap_fail_times = 0;

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    s_heap_calls++;
    if ((caps & MALLOC_CAP_SPIRAM) != 0) {
        s_heap_spiram_calls++;
    }
    if (s_heap_fail_times > 0) {
        s_heap_fail_times--;
        return NULL;
    }
    return malloc(size);
}

void wake_mock_fail_heap_caps_malloc(int times)
{
    s_heap_fail_times = times;
}

int wake_mock_heap_caps_calls(void)
{
    return s_heap_calls;
}

int wake_mock_heap_caps_spiram_calls(void)
{
    return s_heap_spiram_calls;
}

/* --- Время -------------------------------------------------------------- */

static int64_t s_now_us = 0;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void wake_mock_advance_us(int64_t delta_us)
{
    s_now_us += delta_us;
}

int64_t wake_mock_now_us(void)
{
    return s_now_us;
}

void wake_mock_reset_time(void)
{
    s_now_us = 0;
}

/* --- Общий сброс -------------------------------------------------------- */

void wake_mock_reset(void)
{
    s_sem.taken = 0;
    s_sem.level = 0;
    s_sem_creates = 0;
    s_sem_takes = 0;
    s_sem_gives = 0;
    memset(&s_task, 0, sizeof(s_task));
    s_task_handle = NULL;
    s_fail_next_task_create = false;
    s_task_creates = 0;
    s_fail_next_rfft_init = false;
    s_rfft_inits = 0;
    s_rfft_runs = 0;
    s_heap_calls = 0;
    s_heap_spiram_calls = 0;
    s_heap_fail_times = 0;
    s_now_us = 0;
}
