#ifndef APP_OTA_HPP
#define APP_OTA_HPP

#include <atomic>
#include <cstddef>

// C-совместимый заголовок фасада: подписи kouki_ota_* и типы esp_err_t
// остаются общими для C- и C++-модулей.
#include "app_ota.h"

// FreeRTOS.h обязан идти раньше event_groups.h — то же требование, что в
// IDF-заголовках (иначе event_groups.h падает с #error на include guard).
#include "freertos/FreeRTOS.h"

#include "freertos/event_groups.h"

// Тип handle нужен подписи колбэка esp_https_ota (как в app_http.h).
#include "esp_http_client.h"

namespace kouki {

class Config;
class Wifi;
class Http;

/**
 * OTA-обновления прошивки станции (T121 П8).
 *
 * Владеет состоянием модуля: признаком PENDING_VERIFY (новый образ ждёт
 * подтверждения связи с сервером), моментом старта ожидания, флагом «таймаут
 * уже в логе», признаком идущей записи, группой событий старта/внеочередной
 * проверки и задачей опроса. Все они — члены класса, а не static-переменные
 * файла.
 *
 * Зависимости приходят в конструктор: настройки (srv_url, srv_token, pinned
 * CA, интервал опроса) — из Config; Wifi и Http принимаются по спеке §2 как
 * зависимости модуля. Своего обращения к ним у OTA-пути нет: признак связи
 * приходит сигналом старта из main.c (как и в C-модуле), а HTTP-обмен идёт
 * через esp_http_client/esp_https_ota напрямую — построчно как в master,
 * чтобы порядок вызовов и тексты логов не поехали.
 *
 * Экземпляр один (см. kouki::ota()), создаётся статически при первом
 * обращении; деструктора нет — модуль живёт до перезагрузки.
 */
class Ota {
public:
    Ota(Config &config, Wifi &wifi, Http &http);
    Ota(const Ota &) = delete;
    Ota &operator=(const Ota &) = delete;

    /**
     * Проверяет состояние образа у bootloader (PENDING_VERIFY после OTA),
     * создаёт группу событий и задачу опроса обновлений.
     */
    esp_err_t init();

    /** Разрешает задаче опрос (звать после успешного подключения к сети). */
    void start();

    /** Просит немедленную проверку обновлений (кнопка в UI). */
    void check_now();

    /**
     * Подтверждает валидность образа после первого успешного обмена с
     * сервером: в PENDING_VERIFY зовёт esp_ota_mark_app_valid_cancel_rollback().
     * Повторный вызов — no-op.
     */
    void mark_server_communicated();

    /** Идёт ли сейчас загрузка/запись образа. */
    bool is_updating() const;

    /**
     * Шаг ожидания PENDING_VERIFY: задача OTA зовёт его раз в секунду, а если
     * за 60 с связи с сервером не было — один раз пишет предупреждение.
     * Отдельный метод, чтобы host-стенд проверял таймаут без запуска задачи.
     */
    void poll_verify_timeout();

    /**
     * Установка образа по URL с проверкой sha256: ждёт покоя станции (IDLE),
     * качает, сверяет длину и хеш, ставит в слот и перезагружается.
     * Зовётся из проверки обновлений; отдельным методом — чтобы host-стенд
     * мог пройти ветки ошибок (begin/perform/неполный образ/чужой sha256).
     */
    esp_err_t update(const char *image_url, const char *expected_sha256);

    /**
     * Один цикл опроса сервера: GET /api/firmware?current=<версия>, разбор
     * ответа и, если версия новее, установка. Задача OTA зовёт его по
     * расписанию; отдельным методом — чтобы host-стенд проходил ветки разбора
     * ответа без бесконечного цикла задачи.
     */
    void poll_once();

    /**
     * Сравнивает две строки semver ("0.1.0" / "v0.1.1-rc"): >0, 0, <0.
     * Состояния не требует, поэтому статический — фасад kouki_ota_semver_compare
     * зовёт его без экземпляра.
     */
    static int semver_compare(const char *v1, const char *v2);

private:
    /** Статическая trampoline-функция задачи FreeRTOS: зовёт run_task(). */
    static void ota_task_trampoline(void *arg);

    /** Тело задачи опроса: старт по сигналу, пауза, цикл проверок. */
    void run_task();

    /** Колбэк esp_https_ota: подставляет Authorization из Config. */
    static esp_err_t http_client_init_trampoline(esp_http_client_handle_t client);
    esp_err_t on_http_client_init(esp_http_client_handle_t client);

    Config &m_config;
    Wifi &m_wifi;
    Http &m_http;

    bool m_pending_verify;
    TickType_t m_verify_start_tick;
    bool m_verify_timeout_logged;
    // Признак читает UI из другой задачи — как и прежний volatile bool.
    std::atomic<bool> m_is_updating;

    TaskHandle_t m_task;
    EventGroupHandle_t m_events;
};

/** Единственный экземпляр модуля OTA. */
Ota &ota();

}  // namespace kouki

#endif  // APP_OTA_HPP
