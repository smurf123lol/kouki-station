#pragma once

/*
 * Заглушка dl_rfft.h (esp-dl) для host-стенда Wake (T121 П9).
 *
 * Формат входа/выхода тот же, что у IDF-реализации: на входе 512 вещественных
 * сэмплов, на выходе спектр в упакованном виде — data[0] = Re(k=0),
 * data[1] = Re(k=N/2), иначе data[2k] = Re(k), data[2k+1] = Im(k).
 * Саму реализацию (наивный DFT) даёт wake_mock.c.
 *
 * Граница доказательства: на стенде FFT заменён честным DFT с тем же форматом
 * выхода, поэтому mel-спектр и инференс модели считаются по-настоящему, но
 * значения могут отличаться от esp-dl на уровне погрешности float.
 */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_fft_f32_t dl_fft_f32_t;

dl_fft_f32_t *dl_rfft_f32_init(int fft_point, uint32_t caps);
void dl_rfft_f32_deinit(dl_fft_f32_t *handle);
esp_err_t dl_rfft_f32_run(dl_fft_f32_t *handle, float *data);

/** Заказать NULL на следующем dl_rfft_f32_init (имитация нехватки памяти). */
void wake_mock_fail_next_rfft_init(void);

/** Сколько FFT-инстансов создано за тест. */
int wake_mock_rfft_inits(void);

/** Сколько раз запускали FFT (стенд видит, что инференс реально считался). */
int wake_mock_rfft_runs(void);

#ifdef __cplusplus
}
#endif
