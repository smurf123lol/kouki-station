#pragma once

/*
 * Заглушка esp_timer.h для host-стенда Wake (T121 П9).
 *
 * app_wake.cpp читает только монотонное время (mute до какого момента), поэтому
 * в стенде время — переменная, которую двигает тест через
 * wake_mock_advance_us(). Так проверяются mute: пока время внутри окна —
 * хита нет, после окна — детектор снова работает.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int64_t esp_timer_get_time(void);

/** Сдвинуть «часы» вперёд на delta_us микросекунд. */
void wake_mock_advance_us(int64_t delta_us);

/** Текущее «время» стенда (мкс с начала теста). */
int64_t wake_mock_now_us(void);

/** Сброс «часов» в ноль. */
void wake_mock_reset_time(void);

#ifdef __cplusplus
}
#endif
