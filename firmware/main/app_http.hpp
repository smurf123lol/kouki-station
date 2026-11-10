#ifndef APP_HTTP_HPP
#define APP_HTTP_HPP

#include <cstddef>

// C-совместимый заголовок фасада: подписи kouki_http_* и типы esp_http_client_*
// остаются общими для C- и C++-модулей.
#include "app_http.h"

namespace kouki {

class Config;

/**
 * HTTP-клиент станции (T121 П4).
 *
 * Модуль без собственного состояния: все данные приходят аргументами, поэтому
 * у класса только методы, а член — ссылка на конфигурацию (pinned CA). Ссылка
 * приходит в конструктор, а не через kouki_config_get_*: спрятанных вызовов
 * соседей через C-фасад нет.
 *
 * Экземпляр один (см. kouki::http()), создаётся статически — без new/delete
 * и без деструкторов.
 */
class Http {
public:
    explicit Http(Config &config);

    Http(const Http &) = delete;
    Http &operator=(const Http &) = delete;

    /**
     * Создаёт esp_http_client_handle_t:
     * - привязывает pinned CA (Config::effective_ca_pem()) для https://;
     * - подставляет таймаут TLS из CONFIG_KOUKI_TLS_TIMEOUT_S, если он не задан;
     * - отбивает http:// кодом ESP_ERR_NOT_SUPPORTED, если CONFIG_KOUKI_ALLOW_HTTP=n;
     * - ставит логгер успешной проверки сертификата на HTTP_EVENT_ON_CONNECTED.
     */
    esp_err_t new_client(const esp_http_client_config_t *base_cfg,
                         esp_http_client_handle_t *out_client);

    /** Логирует успешное завершение рукопожатия TLS для соединений по https://. */
    void on_connected(const char *url);

    /**
     * Диагностирует причину ошибки соединения и пишет текст в err_buf:
     * сбой проверки сертификата, сбой DNS или «нет связи».
     */
    void diagnose_error(esp_http_client_handle_t client, const char *url, esp_err_t req_err,
                        char *err_buf, size_t max_len);

private:
    Config &m_config;
};

/** Единственный экземпляр HTTP-клиента. */
Http &http();

}  // namespace kouki

#endif  // APP_HTTP_HPP
