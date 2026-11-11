#ifndef KOUKI_TEST_LOG_CAPTURE_H
#define KOUKI_TEST_LOG_CAPTURE_H

/*
 * Перехват ESP_LOG* в host-стендах (T121 П14).
 *
 * По умолчанию заглушки esp_log.h содержат `if (0) printf(...)`: вызовы
 * компилируются, но тексты логов никуда не попадают, и проверить их можно
 * только чтением исходников. Этого мало там, где важна ПРИЧИНА ветки, а не
 * её наличие: код ответа 401, отказ inactive_time, ветка ошибки сохранения
 * в NVS. Тогда стенд собирается с -DKOUKI_TEST_LOG_CAPTURE, и те же макросы
 * складывают форматированную строку в буфер этого модуля.
 *
 * Буфер один на процесс, хранит последние kouki_test_log_capacity байт: тесты
 * чистят его перед проверяемым вызовом через kouki_test_log_clear().
 */

#ifdef KOUKI_TEST_LOG_CAPTURE

#ifdef __cplusplus
extern "C" {
#endif

/** Дописывает строку «УРОВЕНЬ тег: текст\n» в буфер перехвата. */
void kouki_test_log_emit(char level, const char *tag, const char *fmt, ...);

/** Забыть накопленные строки. */
void kouki_test_log_clear(void);

/** Весь накопленный текст (пустая строка, если ничего не логировалось). */
const char *kouki_test_log_text(void);

/** Сколько строк перехвачено с последней очистки. */
int kouki_test_log_count(void);

/** Есть ли подстрока в перехваченном тексте (1/0). */
int kouki_test_log_contains(const char *needle);

#ifdef __cplusplus
}
#endif

#endif  // KOUKI_TEST_LOG_CAPTURE

#endif  // KOUKI_TEST_LOG_CAPTURE_H
