#ifndef APP_SETUP_HPP
#define APP_SETUP_HPP

#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: kouki_setup_* остаются точкой входа для main.c.
#include "app_setup.h"

#include "esp_err.h"
// freeRTOS.h обязан идти до заголовков IDF, которые его подразумевают
// (esp_http_server.h тянет event_groups/list).
#include "freertos/FreeRTOS.h"
#include "esp_http_server.h"

namespace kouki {

class Config;
class Wifi;

/**
 * Режим первичной настройки станции (T121 П11).
 *
 * Владеет состоянием модуля: активностью портала и сервером настройки.
 * Консольные команды и HTTP-обработчики — методы экземпляра; для C-API
 * httpd-обработчики остаются статическими trampoline-функциями, которые
 * достают экземпляр из req->user_ctx (спека §5).
 *
 * SoftAP поднимается напрямую через esp_wifi_*: режим точки доступа — это не
 * клиентское подключение kouki::Wifi, а отдельный сценарий (пароль-открытая
 * сеть, канал 1). Ссылка на Wifi нужна как точка стыка модулей (спека §2) и
 * как доказательство, что режим STA в этот момент не используется.
 */
class Setup {
public:
    Setup(Config &config, Wifi &wifi);

    // Точка входа портала настройки: SoftAP «Kouki-Setup-XXXX» + httpd:80.
    esp_err_t start();

    bool is_active() const;

    // Фоновая консоль USB/jtag. Отдельный метод, чтобы C-фасад оставался
    // однострочником, а тест мог поднять её без драйвера.
    esp_err_t console_init();

    /**
     * Разбор одной строки консоли. Публичный, потому что это единственная
     * точка, через которую проверяются команды настройки (host-стенд подаёт
     * строки напрямую). В C-API не входит.
     */
    void handle_console_command(const char *cmd);

    /**
     * Таблица маршрутов портала — то, что регистрируется в httpd.
     * user_ctx каждого маршрута указывает на этот экземпляр, поэтому таблица
     * не статическая: у класса один экземпляр, но адрес должен быть его.
     */
    static size_t route_count();
    const httpd_uri_t *routes() const;

private:
    // Обработчики: статические trampoline для httpd_uri_t + методы на this.
    static esp_err_t index_get_entry(httpd_req_t *req);
    static esp_err_t save_post_entry(httpd_req_t *req);
    static void restart_entry(void *arg);
    static void console_entry(void *arg);

    esp_err_t index_get(httpd_req_t *req);
    esp_err_t save_post(httpd_req_t *req);
    void delayed_restart();
    void console_loop();

    Config &m_config;
    Wifi &m_wifi;
    httpd_handle_t m_server = nullptr;
    httpd_uri_t m_routes[3] = {};
    bool m_active = false;
};

/** Единственный экземпляр на образ (без new/delete). */
Setup &setup();

}  // namespace kouki

#endif  // APP_SETUP_HPP
