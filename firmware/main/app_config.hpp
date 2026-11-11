#ifndef APP_CONFIG_HPP
#define APP_CONFIG_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// C-совместимый заголовок фасада: типы kouki_config_t и константы
// KOUKI_CONFIG_* остаются общими для C- и C++-модулей.
#include "app_config.h"

namespace kouki {

/**
 * Конфигурация станции (T121 П2).
 *
 * Владеет состоянием модуля: значения в памяти, признак инициализации и
 * атомарные копии «горячих» настроек (тишина, wake-word), которые читаются
 * из других задач без блокировок. NVS namespace и ключи — прежние
 * (`kouki_config`: cfg_ver, wifi_ssid, wifi_pass, srv_url, srv_token,
 * dev_source, silent, wake_en, wake_th, wake_mig, ca_pem, ota_int_s).
 *
 * Экземпляр один на образ (см. kouki::config()), создаётся статически —
 * без new/delete и без деструкторов. init() зовётся из app_main.
 */
class Config {
public:
    Config();
    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    /** Читает настройки из NVS; при отсутствии namespace оставляет дефолты. */
    esp_err_t init();

    /** Заполнена ли конфигурация полностью (ssid, url, token, cfg_ver). */
    bool is_complete() const;

    const char *wifi_ssid() const;
    const char *wifi_pass() const;
    const char *srv_url() const;
    const char *srv_token() const;
    const char *dev_source() const;
    const char *ca_pem() const;
    const char *effective_ca_pem() const;
    uint32_t ota_interval_s() const;

    bool wake_enabled() const;
    esp_err_t set_wake_enabled(bool enabled);
    uint8_t wake_threshold() const;
    esp_err_t set_wake_threshold(uint8_t threshold);

    bool is_silent() const;
    void set_silent(bool silent);

    esp_err_t set_wifi(const char *ssid, const char *pass);
    esp_err_t set_url(const char *url);
    esp_err_t set_token(const char *token);
    esp_err_t set_source(const char *source);
    esp_err_t set_ca_pem(const char *pem);
    esp_err_t set_ota_interval(uint32_t interval_s);

    /** Пишет текущие значения в NVS (cfg_ver = 1); неполную конфигурацию не сохраняет. */
    esp_err_t save();

    /** Стирает namespace kouki_config и сбрасывает значения в памяти. */
    esp_err_t erase();

    /** Сводка конфигурации с маскированием секретов (***). */
    void format_masked(char *buf, size_t max_len) const;

private:
    void set_defaults();

    kouki_config_t m_cfg;
    bool m_initialized;
    // «Горячие» настройки читаются из других задач (FSM/UI) — как и раньше,
    // атомарно, без мьютекса.
    std::atomic<bool> m_silent;
    std::atomic<bool> m_wake_en;
    std::atomic<uint32_t> m_wake_th;
};

/**
 * Единственный экземпляр конфигурации модуля.
 * Фасад kouki_config_* определён рядом, в app_config.cpp: другого места
 * сборки фасадов в П2 нет (решение зафиксировано в отчёте задачи).
 */
Config &config();

}  // namespace kouki

#endif  // APP_CONFIG_HPP
