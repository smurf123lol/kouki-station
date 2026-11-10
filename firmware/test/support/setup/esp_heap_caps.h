#pragma once

/*
 * Заглушка esp_heap_caps.h для host-стенда Setup (T121 П11): консольная
 * команда diag печатает свободную память heap/PSRAM.
 */
#include <stddef.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL 1u
#define MALLOC_CAP_SPIRAM 2u
#define MALLOC_CAP_8BIT 4u

#ifdef __cplusplus
extern "C" {
#endif

void *setup_heap_caps_malloc(size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);
size_t esp_get_free_heap_size(void);
int setup_heap_calls(void);

#ifdef __cplusplus
}
#endif
