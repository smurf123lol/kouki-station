#ifndef APP_POWER_H
#define APP_POWER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KOUKI_POWER_BRIGHTNESS_ACTIVE 100
#define KOUKI_POWER_BRIGHTNESS_DIM    20
#define KOUKI_POWER_BRIGHTNESS_OFF    0

#define KOUKI_POWER_IDLE_DIM_MS       45000   /* 45 секунд до притухания */
#define KOUKI_POWER_IDLE_OFF_MS       180000  /* 180 секунд (3 мин) до отключения */

#define KOUKI_POWER_FADE_DIM_MS       1500    /* 1.5 секунды на димминг */
#define KOUKI_POWER_FADE_OFF_MS       1000    /* 1.0 секунда на затухание в 0 */
#define KOUKI_POWER_FADE_WAKE_MS      50      /* Мгновенный выход на 100% при пробуждении */

/**
 * @brief Инициализация подсистемы питания и подсветки экрана.
 */
void kouki_power_init(void);

/**
 * @brief Уведомление об активности пользователя (тач, кнопка, wake-word).
 * Немедленно восстанавливает максимальную яркость и сбрасывает счетчик неактивности.
 */
void kouki_power_notify_activity(void);

/**
 * @brief Периодический шаг обновления диспетчера питания (вызывается из UI таймера/задачи).
 * 
 * @param dt_ms Прошедшее время в миллисекундах с прошлого тика.
 */
void kouki_power_tick(uint32_t dt_ms);

/**
 * @brief Возвращает текущую установленную яркость дисплея (0..100).
 */
uint8_t kouki_power_get_brightness(void);

/**
 * @brief Возвращает текущее время неактивности в миллисекундах.
 */
uint32_t kouki_power_get_idle_ms(void);

#ifdef __cplusplus
}
#endif

#endif // APP_POWER_H
