#pragma once

/*
 * Заглушка esp_system.h для host-стенда Setup (T121 П11): перезагрузка после
 * сохранения настроек только запоминается.
 */
#include "esp_heap_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

/** Сколько раз просили перезагрузку (общий счётчик стенда Setup). */
int setup_restart_calls(void);
void setup_restart_reset(void);

#ifdef __cplusplus
}
#endif
