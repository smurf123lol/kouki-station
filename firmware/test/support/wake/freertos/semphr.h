#pragma once

/*
 * Заглушка freertos/semphr.h для host-стенда Wake (T121 П9).
 *
 * app_wake.cpp берёт leaf-мьютекс состояния через ленивое создание под
 * критической секцией. Стенд однопоточный, поэтому мьютекс — счётчик
 * занятости: порядок «взял — отпустил» проверяется, взаимные блокировки
 * невозможны. Все функции объявлены в extern "C": мок собран gcc как C,
 * а зовёт его C++-код модуля.
 */
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

/** Сколько раз мьютекс реально брали (стенд видит, что lock не течёт). */
int wake_mock_semaphore_takes(void);

/** Сколько мьютексов создано (стенд видит, что ленивое создание одноразовое). */
int wake_mock_semaphore_creates(void);

/** Сколько раз мьютекс отпускали — баланс с takes ловит забытый unlock. */
int wake_mock_semaphore_gives(void);

/** Держит ли кто-то мьютекс прямо сейчас. */
bool wake_mock_semaphore_held(void);

#ifdef __cplusplus
}
#endif
