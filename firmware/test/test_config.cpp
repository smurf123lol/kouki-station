/*
 * Host-стенд модуля конфигурации (T121 П2, firmware/main/app_config.cpp).
 *
 * Проверяет поведение kouki::Config и C-фасада kouki_config_* на моке NVS:
 * ключи и значения NVS — 1:1 с прежним C-модулем, миграция wake_mig,
 * is_complete(), save()/erase(), маскирование секретов.
 *
 * Настоящего mbedtls на хосте нет: support/config_mock/mbedtls/ подменяет
 * разбор PEM грубой проверкой маркеров (см. комментарий в заглушке).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "app_config.h"
#include "app_config.hpp"
#include "nvs_mock.h"

static int g_checks_run = 0;
static int g_checks_passed = 0;

#define TEST_ASSERT(cond, msg) do {                                    \
    g_checks_run++;                                                    \
    if (!(cond)) {                                                     \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":"        \
                  << __LINE__ << ")" << std::endl;                     \
        std::exit(1);                                                  \
    } else {                                                           \
        g_checks_passed++;                                             \
    }                                                                  \
} while (0)

namespace {

const char *const kNs = KOUKI_CONFIG_NVS_NAMESPACE;

/** Чистый «флеш» + возврат модуля к дефолтам через init() на пустом NVS. */
void reset_module()
{
    nvs_mock_reset();
    const esp_err_t err = kouki_config_init();
    TEST_ASSERT(err == ESP_OK, "init() на пустом NVS возвращает ESP_OK");
}

/** Закладка полной валидной конфигурации в «флеш». */
void seed_full_config(uint8_t wake_th, bool with_wake_mig)
{
    nvs_mock_seed_u8(kNs, "cfg_ver", KOUKI_CONFIG_VERSION);
    nvs_mock_seed_str(kNs, "wifi_ssid", "kouki-home");
    nvs_mock_seed_str(kNs, "wifi_pass", "secret-pass");
    nvs_mock_seed_str(kNs, "srv_url", "http://192.168.1.20:5300");
    nvs_mock_seed_str(kNs, "srv_token", "0123456789abcdef");
    nvs_mock_seed_str(kNs, "dev_source", "kitchen");
    nvs_mock_seed_u32(kNs, "ota_int_s", 3600);
    nvs_mock_seed_u8(kNs, "silent", 1);
    nvs_mock_seed_u8(kNs, "wake_en", 0);
    nvs_mock_seed_u8(kNs, "wake_th", wake_th);
    if (with_wake_mig) {
        nvs_mock_seed_u8(kNs, "wake_mig", 1);
    }
}

// ---------------------------------------------------------------------------
// 1. Пустой NVS: дефолты, создание namespace, неполная конфигурация
// ---------------------------------------------------------------------------
void test_init_empty_flash()
{
    std::cout << "[TEST] 1. init() на пустом NVS: дефолты и namespace..." << std::endl;

    nvs_mock_reset();
    TEST_ASSERT(!nvs_mock_has_namespace(kNs), "до init() namespace kouki_config отсутствует");

    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() на пустом NVS — ESP_OK");
    TEST_ASSERT(std::strcmp(nvs_mock_last_open_namespace(), kNs) == 0,
                "init() открывает namespace kouki_config");
    TEST_ASSERT(nvs_mock_has_namespace(kNs),
                "NVS_READWRITE создаёт namespace (поведение IDF)");

    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "") == 0, "wifi_ssid пуст");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_pass(), "") == 0, "wifi_pass пуст");
    TEST_ASSERT(std::strcmp(kouki_config_get_srv_url(), "") == 0, "srv_url пуст");
    TEST_ASSERT(std::strcmp(kouki_config_get_srv_token(), "") == 0, "srv_token пуст");
    TEST_ASSERT(std::strcmp(kouki_config_get_dev_source(), KOUKI_CONFIG_DEFAULT_SOURCE) == 0,
                "dev_source по умолчанию «station»");
    TEST_ASSERT(std::strcmp(kouki_config_get_ca_pem(), "") == 0, "ca_pem пуст");
    TEST_ASSERT(kouki_config_get_ota_interval_s() == KOUKI_CONFIG_DEFAULT_OTA_INTERVAL,
                "ota_int_s по умолчанию 43200");
    TEST_ASSERT(kouki_config_get_wake_enabled() == (KOUKI_CONFIG_DEFAULT_WAKE_ENABLED != 0),
                "wake_en по умолчанию включён");
    TEST_ASSERT(kouki_config_get_wake_threshold() == KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD,
                "wake_th по умолчанию 65");
    TEST_ASSERT(kouki_config_is_silent() == false, "тишина по умолчанию выключена (Kconfig n)");
    TEST_ASSERT(!kouki_config_is_complete(), "пустая конфигурация неполная");
    TEST_ASSERT(std::strcmp(nvs_mock_write_log(), "") == 0,
                "чтение пустого NVS ничего не пишет");
}

// ---------------------------------------------------------------------------
// 2. Полная конфигурация: все ключи читаются, чтение не пишет
// ---------------------------------------------------------------------------
void test_init_reads_saved_config()
{
    std::cout << "[TEST] 2. init() читает сохранённые ключи NVS..." << std::endl;

    nvs_mock_reset();
    seed_full_config(65, true);

    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() с полной конфигурацией — ESP_OK");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "kouki-home") == 0, "wifi_ssid прочитан");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_pass(), "secret-pass") == 0, "wifi_pass прочитан");
    TEST_ASSERT(std::strcmp(kouki_config_get_srv_url(), "http://192.168.1.20:5300") == 0,
                "srv_url прочитан");
    TEST_ASSERT(std::strcmp(kouki_config_get_srv_token(), "0123456789abcdef") == 0,
                "srv_token прочитан");
    TEST_ASSERT(std::strcmp(kouki_config_get_dev_source(), "kitchen") == 0, "dev_source прочитан");
    TEST_ASSERT(kouki_config_get_ota_interval_s() == 3600, "ota_int_s прочитан");
    TEST_ASSERT(kouki_config_is_silent() == true, "silent прочитан (1)");
    TEST_ASSERT(kouki_config_get_wake_enabled() == false, "wake_en прочитан (0)");
    TEST_ASSERT(kouki_config_get_wake_threshold() == 65, "wake_th прочитан");
    TEST_ASSERT(kouki_config_is_complete(), "полная конфигурация считается полной");
    TEST_ASSERT(std::strcmp(nvs_mock_write_log(), "") == 0,
                "чтение полной конфигурации ничего не пишет в NVS");

    const char *embedded = kouki_config_get_effective_ca_pem();
    TEST_ASSERT(embedded != nullptr && std::strstr(embedded, "-----BEGIN CERTIFICATE-----") != nullptr,
                "без ca_pem в NVS действует вшитый Caddy CA");
}

// ---------------------------------------------------------------------------
// 3. Чужая версия схемы: значения не подхватываются, конфигурация пустая
// ---------------------------------------------------------------------------
void test_init_version_mismatch()
{
    std::cout << "[TEST] 3. init() при чужой cfg_ver оставляет дефолты..." << std::endl;

    nvs_mock_reset();
    seed_full_config(65, true);
    nvs_mock_seed_u8(kNs, "cfg_ver", KOUKI_CONFIG_VERSION + 1);

    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() с чужой версией — ESP_OK");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "") == 0,
                "wifi_ssid не подхвачен при чужой cfg_ver");
    TEST_ASSERT(std::strcmp(kouki_config_get_srv_url(), "") == 0,
                "srv_url не подхвачен при чужой cfg_ver");
    TEST_ASSERT(!kouki_config_is_complete(), "конфигурация неполная");
    TEST_ASSERT(std::strcmp(nvs_mock_write_log(), "") == 0,
                "чужая версия схемы не перезаписывает NVS");
}

// ---------------------------------------------------------------------------
// 4. Миграция wake_mig: старый образ с wake_th >= 90
// ---------------------------------------------------------------------------
void test_wake_migration()
{
    std::cout << "[TEST] 4. миграция wake_mig переводит порог 95 -> 65..." << std::endl;

    nvs_mock_reset();
    seed_full_config(95, false);

    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() со старым образом — ESP_OK");
    TEST_ASSERT(kouki_config_get_wake_threshold() == KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD,
                "порог 95 мигрирован в 65");
    uint8_t stored = 0;
    TEST_ASSERT(nvs_mock_get_u8(kNs, "wake_th", &stored) && stored == KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD,
                "мигрированный порог записан в NVS");
    TEST_ASSERT(nvs_mock_get_u8(kNs, "wake_mig", &stored) && stored == 1,
                "флаг wake_mig выставлен в 1");
    TEST_ASSERT(nvs_mock_was_written(kNs, "wake_th"), "миграция переписала ключ wake_th");
    TEST_ASSERT(nvs_mock_was_written(kNs, "wake_mig"), "миграция записала ключ wake_mig");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "kouki-home") == 0,
                "остальные настройки миграция не портит");

    // Повторный init() на уже мигрированном образе ничего не пишет.
    nvs_mock_reset();
    seed_full_config(95, true);
    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() на мигрированном образе — ESP_OK");
    TEST_ASSERT(kouki_config_get_wake_threshold() == 95,
                "при wake_mig=1 порог 95 сохраняется как есть");
    TEST_ASSERT(std::strcmp(nvs_mock_write_log(), "") == 0,
                "миграция выполняется один раз");
}

// ---------------------------------------------------------------------------
// 5. Некорректный порог в NVS: сброс к дефолту, без миграции
// ---------------------------------------------------------------------------
void test_wake_threshold_invalid_in_nvs()
{
    std::cout << "[TEST] 5. некорректный wake_th в NVS заменяется дефолтом..." << std::endl;

    nvs_mock_reset();
    seed_full_config(5, true);
    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() с порогом 5 — ESP_OK");
    TEST_ASSERT(kouki_config_get_wake_threshold() == KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD,
                "порог ниже 10 заменён на 65");

    nvs_mock_reset();
    seed_full_config(5, false);
    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() с порогом 5 без wake_mig — ESP_OK");
    TEST_ASSERT(kouki_config_get_wake_threshold() == KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD,
                "порог ниже 10 и без wake_mig тоже 65");
    uint8_t stored = 0;
    TEST_ASSERT(nvs_mock_get_u8(kNs, "wake_mig", &stored) && stored == 1,
                "wake_mig выставляется даже когда порог не мигрировался");
}

// ---------------------------------------------------------------------------
// 6. save() неполной конфигурации: отказ и отсутствие записи
// ---------------------------------------------------------------------------
void test_save_incomplete()
{
    std::cout << "[TEST] 6. save() неполной конфигурации отказывает..." << std::endl;

    reset_module();
    TEST_ASSERT(kouki_config_save() == ESP_ERR_INVALID_STATE,
                "save() без ssid/url/token — ESP_ERR_INVALID_STATE");
    TEST_ASSERT(!nvs_mock_has_key(kNs, "cfg_ver"),
                "неполная конфигурация не сохраняет cfg_ver");
    TEST_ASSERT(std::strcmp(nvs_mock_write_log(), "") == 0,
                "неполная конфигурация не пишет в NVS вовсе");

    // Мало токена — тоже неполная.
    TEST_ASSERT(kouki_config_set_wifi("kouki-home", "secret-pass") == ESP_OK, "set_wifi валиден");
    TEST_ASSERT(kouki_config_set_url("http://192.168.1.20:5300") == ESP_OK, "set_url валиден");
    TEST_ASSERT(kouki_config_set_token("short") == ESP_ERR_INVALID_ARG,
                "set_token короче 8 символов отклонён");
    TEST_ASSERT(kouki_config_save() == ESP_ERR_INVALID_STATE,
                "без токена save() по-прежнему отказывает");
}

// ---------------------------------------------------------------------------
// 7. save() полной конфигурации: все ключи, ca_pem стёрт при пустом
// ---------------------------------------------------------------------------
void test_save_complete()
{
    std::cout << "[TEST] 7. save() полной конфигурации пишет все ключи..." << std::endl;

    reset_module();
    TEST_ASSERT(kouki_config_set_wifi("kouki-home", "secret-pass") == ESP_OK, "set_wifi");
    TEST_ASSERT(kouki_config_set_url("http://192.168.1.20:5300") == ESP_OK, "set_url");
    TEST_ASSERT(kouki_config_set_token("0123456789abcdef") == ESP_OK, "set_token");
    TEST_ASSERT(kouki_config_set_source("kitchen") == ESP_OK, "set_source");
    TEST_ASSERT(kouki_config_set_ota_interval(3600) == ESP_OK, "set_ota_interval");
    kouki_config_set_wake_enabled(false);
    TEST_ASSERT(kouki_config_set_wake_threshold(80) == ESP_OK, "set_wake_threshold");
    kouki_config_set_silent(true);

    TEST_ASSERT(kouki_config_save() == ESP_OK, "save() полной конфигурации — ESP_OK");

    uint8_t u8 = 0;
    uint32_t u32 = 0;
    const char *str = nullptr;

    TEST_ASSERT(nvs_mock_get_u8(kNs, "cfg_ver", &u8) && u8 == KOUKI_CONFIG_VERSION, "ключ cfg_ver = 1");
    TEST_ASSERT(nvs_mock_get_str(kNs, "wifi_ssid", &str) && std::strcmp(str, "kouki-home") == 0,
                "ключ wifi_ssid");
    TEST_ASSERT(nvs_mock_get_str(kNs, "wifi_pass", &str) && std::strcmp(str, "secret-pass") == 0,
                "ключ wifi_pass");
    TEST_ASSERT(nvs_mock_get_str(kNs, "srv_url", &str) &&
                std::strcmp(str, "http://192.168.1.20:5300") == 0, "ключ srv_url");
    TEST_ASSERT(nvs_mock_get_str(kNs, "srv_token", &str) && std::strcmp(str, "0123456789abcdef") == 0,
                "ключ srv_token");
    TEST_ASSERT(nvs_mock_get_str(kNs, "dev_source", &str) && std::strcmp(str, "kitchen") == 0,
                "ключ dev_source");
    TEST_ASSERT(nvs_mock_get_u32(kNs, "ota_int_s", &u32) && u32 == 3600, "ключ ota_int_s");
    TEST_ASSERT(nvs_mock_get_u8(kNs, "silent", &u8) && u8 == 1, "ключ silent = 1");
    TEST_ASSERT(nvs_mock_get_u8(kNs, "wake_en", &u8) && u8 == 0, "ключ wake_en = 0");
    TEST_ASSERT(nvs_mock_get_u8(kNs, "wake_th", &u8) && u8 == 80, "ключ wake_th = 80");
    TEST_ASSERT(!nvs_mock_has_key(kNs, "ca_pem"), "пустой ca_pem удалён из NVS (nvs_erase_key)");

    // Перечитывание после «перезагрузки»: NVS на месте, init() снова читает ключи.
    TEST_ASSERT(kouki_config_init() == ESP_OK, "init() после «перезагрузки»");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "kouki-home") == 0, "ssid перечитан");
    TEST_ASSERT(kouki_config_get_wake_threshold() == 80, "порог 80 перечитан");
    TEST_ASSERT(kouki_config_is_silent() == true, "silent перечитан");
    TEST_ASSERT(kouki_config_get_wake_enabled() == false, "wake_en перечитан");
    TEST_ASSERT(kouki_config_is_complete(), "перечитанная конфигурация полная");
}

// ---------------------------------------------------------------------------
// 8. Валидаторы сеттеров и «горячие» геттеры
// ---------------------------------------------------------------------------
void test_setters_validation()
{
    std::cout << "[TEST] 8. валидаторы сеттеров и геттеры..." << std::endl;

    reset_module();

    char long_ssid[40];
    std::memset(long_ssid, 'a', sizeof(long_ssid));
    long_ssid[sizeof(long_ssid) - 1] = '\0';
    TEST_ASSERT(kouki_config_set_wifi(long_ssid, "x") == ESP_ERR_INVALID_ARG,
                "set_wifi: ssid длиннее 32 отклонён");
    TEST_ASSERT(kouki_config_set_wifi("", "x") == ESP_ERR_INVALID_ARG,
                "set_wifi: пустой ssid отклонён");
    TEST_ASSERT(kouki_config_set_wifi("kouki-home", nullptr) == ESP_OK,
                "set_wifi: nullptr-пароль допустим");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_pass(), "") == 0,
                "set_wifi: nullptr-пароль даёт пустой пароль");

    TEST_ASSERT(kouki_config_set_url("ftp://example") == ESP_ERR_INVALID_ARG,
                "set_url: схема ftp отклонена");
    TEST_ASSERT(kouki_config_set_url("") == ESP_ERR_INVALID_ARG, "set_url: пустой url отклонён");
    TEST_ASSERT(kouki_config_set_url("http://192.168.1.20:5300") == ESP_OK,
                "set_url: http допустим при CONFIG_KOUKI_ALLOW_HTTP");

    TEST_ASSERT(kouki_config_set_wake_threshold(9) == ESP_ERR_INVALID_ARG, "порог 9 отклонён");
    TEST_ASSERT(kouki_config_set_wake_threshold(96) == ESP_ERR_INVALID_ARG, "порог 96 отклонён");
    TEST_ASSERT(kouki_config_set_wake_threshold(10) == ESP_OK, "порог 10 принят");
    TEST_ASSERT(kouki_config_get_wake_threshold() == 10, "порог 10 читается");
    TEST_ASSERT(kouki_config_set_wake_threshold(95) == ESP_OK, "порог 95 принят");
    TEST_ASSERT(kouki_config_get_wake_threshold() == 95, "порог 95 читается");

    kouki_config_set_wake_enabled(true);
    TEST_ASSERT(kouki_config_get_wake_enabled() == true, "wake_en переключается");
    kouki_config_set_silent(true);
    TEST_ASSERT(kouki_config_is_silent() == true, "silent переключается");

    TEST_ASSERT(kouki_config_set_ota_interval(0) == ESP_OK, "set_ota_interval(0) — ESP_OK");
    TEST_ASSERT(kouki_config_get_ota_interval_s() == KOUKI_CONFIG_DEFAULT_OTA_INTERVAL,
                "set_ota_interval(0) откатывается к 43200");

    TEST_ASSERT(kouki_config_set_source("") == ESP_OK, "пустой source — ESP_OK");
    TEST_ASSERT(std::strcmp(kouki_config_get_dev_source(), KOUKI_CONFIG_DEFAULT_SOURCE) == 0,
                "пустой source даёт «station»");
    TEST_ASSERT(kouki_config_set_source("kitchen") == ESP_OK, "set_source");
    TEST_ASSERT(std::strcmp(kouki_config_get_dev_source(), "kitchen") == 0, "source прочитан");
}

// ---------------------------------------------------------------------------
// 9. ca_pem: валидация PEM и effective_ca_pem
// ---------------------------------------------------------------------------
void test_ca_pem()
{
    std::cout << "[TEST] 9. ca_pem: разбор PEM и фолбэк на вшитый CA..." << std::endl;

    reset_module();
    TEST_ASSERT(kouki_config_set_ca_pem("не сертификат") == ESP_ERR_INVALID_ARG,
                "ca_pem без PEM-маркеров отклонён");
    TEST_ASSERT(kouki_config_set_ca_pem("-----BEGIN CERTIFICATE-----\nЭТО-НЕ-BASE64\n-----END CERTIFICATE-----\n")
                == ESP_ERR_INVALID_ARG, "ca_pem с мусором в теле отклонён");

    char pem[256];
    std::snprintf(pem, sizeof(pem),
                  "-----BEGIN CERTIFICATE-----\n"
                  "MIIBozCCAUmgAwIBAgIQbGdVNmq1SQsDHWfLavqiXDAKBggqhkjOPQQDAjAw\n"
                  "-----END CERTIFICATE-----\n");
    TEST_ASSERT(kouki_config_set_ca_pem(pem) == ESP_OK, "валидный PEM принят");
    TEST_ASSERT(std::strcmp(kouki_config_get_ca_pem(), pem) == 0, "ca_pem прочитан");
    TEST_ASSERT(std::strcmp(kouki_config_get_effective_ca_pem(), pem) == 0,
                "effective_ca_pem возвращает пользовательский CA");

    TEST_ASSERT(kouki_config_set_ca_pem("") == ESP_OK, "пустая строка очищает ca_pem");
    TEST_ASSERT(std::strcmp(kouki_config_get_ca_pem(), "") == 0, "ca_pem очищен");
    TEST_ASSERT(std::strstr(kouki_config_get_effective_ca_pem(), "-----BEGIN CERTIFICATE-----") != nullptr,
                "после очистки ca_pem снова действует вшитый CA");
}

// ---------------------------------------------------------------------------
// 10. format_masked: секреты маскируются
// ---------------------------------------------------------------------------
void test_format_masked()
{
    std::cout << "[TEST] 10. format_masked маскирует секреты..." << std::endl;

    reset_module();
    TEST_ASSERT(kouki_config_set_wifi("kouki-home", "secret-pass") == ESP_OK, "set_wifi");
    TEST_ASSERT(kouki_config_set_url("http://192.168.1.20:5300") == ESP_OK, "set_url");
    TEST_ASSERT(kouki_config_set_token("0123456789abcdef") == ESP_OK, "set_token");

    char buf[1024];
    kouki_config_format_masked(buf, sizeof(buf));

    TEST_ASSERT(std::strstr(buf, "wifi_ssid: ***") != nullptr, "ssid замаскирован");
    TEST_ASSERT(std::strstr(buf, "wifi_pass: ***") != nullptr, "пароль замаскирован");
    TEST_ASSERT(std::strstr(buf, "srv_token: ***") != nullptr, "токен замаскирован");
    TEST_ASSERT(std::strstr(buf, "secret-pass") == nullptr, "пароль не утёк в сводку");
    TEST_ASSERT(std::strstr(buf, "0123456789abcdef") == nullptr, "токен не утёк в сводку");
    TEST_ASSERT(std::strstr(buf, "kouki-home") == nullptr, "ssid не утёк в сводку");
    TEST_ASSERT(std::strstr(buf, "srv_url: http://192.168.1.20:5300") != nullptr,
                "url показан открыто");
    TEST_ASSERT(std::strstr(buf, "dev_source: station") != nullptr, "источник показан");
    TEST_ASSERT(std::strstr(buf, "wake_th: 65") != nullptr, "порог wake показан");
    TEST_ASSERT(std::strstr(buf, "ota_int_s: 43200") != nullptr, "интервал OTA показан");

    char tiny[8];
    std::memset(tiny, 'x', sizeof(tiny));
    kouki_config_format_masked(tiny, sizeof(tiny));
    TEST_ASSERT(tiny[sizeof(tiny) - 1] == '\0', "format_masked не выходит за буфер");

    char guard[1];
    guard[0] = 'q';
    kouki_config_format_masked(guard, 0);
    TEST_ASSERT(guard[0] == 'q', "format_masked с max_len=0 ничего не пишет");
}

// ---------------------------------------------------------------------------
// 11. erase(): очистка NVS и возврат к дефолтам
// ---------------------------------------------------------------------------
void test_erase()
{
    std::cout << "[TEST] 11. erase() очищает NVS и возвращает дефолты..." << std::endl;

    reset_module();
    TEST_ASSERT(kouki_config_set_wifi("kouki-home", "secret-pass") == ESP_OK, "set_wifi");
    TEST_ASSERT(kouki_config_set_url("http://192.168.1.20:5300") == ESP_OK, "set_url");
    TEST_ASSERT(kouki_config_set_token("0123456789abcdef") == ESP_OK, "set_token");
    TEST_ASSERT(kouki_config_save() == ESP_OK, "save() перед erase()");
    TEST_ASSERT(nvs_mock_count_keys(kNs) > 0, "перед erase() в NVS есть ключи");

    TEST_ASSERT(kouki_config_erase() == ESP_OK, "erase() — ESP_OK");
    TEST_ASSERT(nvs_mock_count_keys(kNs) == 0, "после erase() в NVS не осталось ключей");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "") == 0, "после erase() ssid пуст");
    TEST_ASSERT(std::strcmp(kouki_config_get_srv_url(), "") == 0, "после erase() url пуст");
    TEST_ASSERT(std::strcmp(kouki_config_get_dev_source(), KOUKI_CONFIG_DEFAULT_SOURCE) == 0,
                "после erase() источник снова «station»");
    TEST_ASSERT(kouki_config_get_ota_interval_s() == KOUKI_CONFIG_DEFAULT_OTA_INTERVAL,
                "после erase() интервал OTA снова 43200");
    TEST_ASSERT(!kouki_config_is_complete(), "после erase() конфигурация неполная");
}

// ---------------------------------------------------------------------------
// 12. Ошибка открытия NVS: init() возвращает код, дефолты остаются
// ---------------------------------------------------------------------------
void test_init_open_error()
{
    std::cout << "[TEST] 12. init() при ошибке nvs_open возвращает код ошибки..." << std::endl;

    nvs_mock_reset();
    nvs_mock_fail_next_open(ESP_ERR_NVS_NOT_INITIALIZED);
    const esp_err_t err = kouki_config_init();
    TEST_ASSERT(err == ESP_ERR_NVS_NOT_INITIALIZED,
                "init() пробрасывает код nvs_open как есть");
    TEST_ASSERT(std::strcmp(kouki_config_get_dev_source(), KOUKI_CONFIG_DEFAULT_SOURCE) == 0,
                "после ошибки открытия дефолты на месте");
    TEST_ASSERT(kouki_config_get_wake_threshold() == KOUKI_CONFIG_DEFAULT_WAKE_THRESHOLD,
                "после ошибки открытия порог = 65");
}

// ---------------------------------------------------------------------------
// 13. C-фасад и класс работают с одним состоянием
// ---------------------------------------------------------------------------
void test_facade_and_instance_share_state()
{
    std::cout << "[TEST] 13. C-фасад и kouki::config() — одно состояние..." << std::endl;

    reset_module();
    kouki::Config &instance = kouki::config();
    TEST_ASSERT(&instance == &kouki::config(), "kouki::config() возвращает один экземпляр");

    instance.set_silent(true);
    TEST_ASSERT(kouki_config_is_silent() == true, "фасад видит состояние экземпляра");

    kouki_config_set_wake_threshold(70);
    TEST_ASSERT(instance.wake_threshold() == 70, "экземпляр видит состояние фасада");

    TEST_ASSERT(instance.set_wifi("kouki-home", "secret-pass") == ESP_OK, "метод экземпляра set_wifi");
    TEST_ASSERT(std::strcmp(kouki_config_get_wifi_ssid(), "kouki-home") == 0,
                "фасад читает ssid, выставленный через экземпляр");
}

// ---------------------------------------------------------------------------
// 14. Копия вшитого CA совпадает с файлом сертификата
// ---------------------------------------------------------------------------
void test_embedded_cert_matches_file()
{
    std::cout << "[TEST] 14. копия вшитого CA совпадает с certs/kouki-local-root.pem..." << std::endl;

#ifndef KOUKI_TEST_CERT_PATH
    // Запасной путь: __FILE__ указывает на firmware/test/test_config.cpp,
    // если стенд собирали относительным путём из каталога firmware/.
    std::string test_file(__FILE__);
    const size_t slash = test_file.find_last_of('/');
    const std::string test_dir = slash == std::string::npos ? "." : test_file.substr(0, slash);
    const std::string pem_path = test_dir + "/../main/certs/kouki-local-root.pem";
#else
    const std::string pem_path = KOUKI_TEST_CERT_PATH;
#endif

    std::FILE *fp = std::fopen(pem_path.c_str(), "rb");
    TEST_ASSERT(fp != nullptr, "файл сертификата открывается");
    char file_pem[4096];
    const size_t read = std::fread(file_pem, 1, sizeof(file_pem) - 1, fp);
    std::fclose(fp);
    file_pem[read] = '\0';

    const char *embedded = kouki_config_get_effective_ca_pem();
    TEST_ASSERT(std::strcmp(embedded, file_pem) == 0,
                "стендовая копия CA побайтно равна firmware/main/certs/kouki-local-root.pem");
}

}  // namespace

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "   HOST CHECK: T121 Config (П2)         " << std::endl;
    std::cout << "========================================" << std::endl;

    test_init_empty_flash();
    test_init_reads_saved_config();
    test_init_version_mismatch();
    test_wake_migration();
    test_wake_threshold_invalid_in_nvs();
    test_save_incomplete();
    test_save_complete();
    test_setters_validation();
    test_ca_pem();
    test_format_masked();
    test_erase();
    test_init_open_error();
    test_facade_and_instance_share_state();
    test_embedded_cert_matches_file();

    std::cout << "========================================" << std::endl;
    std::cout << "ИТОГО: " << g_checks_passed << "/" << g_checks_run
              << " проверок успешно пройдено!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
