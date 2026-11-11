// Host-стенд T121 П4: класс kouki::Http и его C-фасад.
//
// Сеть не поднимается: esp_http_client заменён моком (support/http_mock/),
// который запоминает конфигурацию и отдаёт заказанную TLS-ошибку. Проверяется
// поведение прежнего C-модуля 1:1: коды esp_err_t, подстановка pinned CA и
// таймаута, диагностика ошибок (сертификат / DNS / связь) и сценарий
// «вызов -> ошибка -> повтор».
//
// Один и тот же файл собирается дважды: с CONFIG_KOUKI_ALLOW_HTTP=1 (sim) и
// с =0 (дефолт Kconfig) — во втором случае http:// обязан отбиваться.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.hpp"
#include "app_http.h"
#include "log_capture.h"
#include "app_http.hpp"
#include "esp_http_client.h"
#include "esp_tls_errors.h"

#ifndef HTTP_EXPECT_ALLOW_HTTP
#define HTTP_EXPECT_ALLOW_HTTP 1
#endif

static int g_checks = 0;
static int g_passed = 0;

#define CHECK(cond, msg) do {                                    \
    g_checks++;                                                  \
    if (!(cond)) {                                               \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        std::exit(1);                                            \
    }                                                            \
    g_passed++;                                                  \
} while (0)

static bool str_eq(const char *a, const char *b)
{
    return a != NULL && b != NULL && std::strcmp(a, b) == 0;
}

// ------------------------------------------------------- создание клиента --

static void test_new_client_args()
{
    std::printf("[TEST] 1. new_client: аргументы, коды ошибок...\n");

    esp_http_client_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://example.test/api/voice";

    esp_http_client_handle_t client = NULL;

    // NULL-аргументы отвергаются прежним кодом.
    CHECK(kouki_http_new_client(NULL, &client) == ESP_ERR_INVALID_ARG,
          "new_client(NULL, &client) -> ESP_ERR_INVALID_ARG");
    CHECK(kouki_http_new_client(&cfg, NULL) == ESP_ERR_INVALID_ARG,
          "new_client(&cfg, NULL) -> ESP_ERR_INVALID_ARG");
    CHECK(http_mock_init_calls() == 0, "no client created on invalid args");

    // Ошибка создания клиента -> ESP_ERR_NO_MEM, повтор после неё проходит.
    http_mock_fail_next_init();
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_ERR_NO_MEM,
          "init failure -> ESP_ERR_NO_MEM");
    CHECK(client == NULL, "out_client untouched on failure");

    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "retry after failure -> ESP_OK");
    CHECK(client != NULL, "client created on retry");
    CHECK(http_mock_init_calls() == 2, "init called twice (failure + retry)");
    esp_http_client_cleanup(client);
}

// --------------------------------------------------- https: CA и таймауты --

static void test_new_client_https()
{
    std::printf("[TEST] 2. new_client: pinned CA, таймаут TLS, event_handler...\n");

    http_mock_reset();

    esp_http_client_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://example.test/api/voice";
    cfg.timeout_ms = 0;  // «не задан» -> модуль подставляет CONFIG_KOUKI_TLS_TIMEOUT_S

    esp_http_client_handle_t client = NULL;
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "https client -> ESP_OK");

    const esp_http_client_config_t *seen = http_mock_last_config();
    CHECK(seen != NULL, "config captured by mock");
    CHECK(seen->timeout_ms == 10 * 1000, "timeout_ms defaulted to CONFIG_KOUKI_TLS_TIMEOUT_S*1000");
    CHECK(seen->cert_pem != NULL, "cert_pem set for https");
    CHECK(str_eq(seen->cert_pem, kouki::config().effective_ca_pem()),
          "cert_pem is Config::effective_ca_pem()");
    CHECK(seen->event_handler != NULL, "default event_handler installed");
    /* Мок хранит копию строки url (см. http_mock.c: модуль собирает URL в
     * локальном буфере, и смотреть на него после возврата — UB). Поэтому
     * проверяем неизменность содержимого, а не совпадение указателей. */
    CHECK(str_eq(seen->url, cfg.url), "url passed through unchanged");
    esp_http_client_cleanup(client);

    // Явно заданный таймаут не перетирается.
    http_mock_reset();
    cfg.timeout_ms = 2500;
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "https client with explicit timeout");
    seen = http_mock_last_config();
    CHECK(seen != NULL && seen->timeout_ms == 2500, "explicit timeout_ms preserved");
    esp_http_client_cleanup(client);

    // Свой event_handler не подменяется.
    http_mock_reset();
    cfg.timeout_ms = 0;
    cfg.event_handler = [](esp_http_client_event_t *) -> esp_err_t { return ESP_OK; };
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "https client with own handler");
    seen = http_mock_last_config();
    CHECK(seen != NULL && seen->event_handler == cfg.event_handler,
          "caller event_handler preserved");
    esp_http_client_cleanup(client);

    // http:// при CONFIG_KOUKI_ALLOW_HTTP=1 проходит, CA не подставляется.
    http_mock_reset();
    cfg.event_handler = NULL;
    cfg.url = "http://192.168.1.10:5300/api/voice";
    client = NULL;
    esp_err_t err = kouki_http_new_client(&cfg, &client);
#if HTTP_EXPECT_ALLOW_HTTP
    CHECK(err == ESP_OK, "http:// allowed when CONFIG_KOUKI_ALLOW_HTTP=1");
    seen = http_mock_last_config();
    CHECK(seen != NULL && seen->cert_pem == NULL, "no pinned CA for http://");
    esp_http_client_cleanup(client);
#else
    CHECK(err == ESP_ERR_NOT_SUPPORTED, "http:// rejected when CONFIG_KOUKI_ALLOW_HTTP=0");
    CHECK(client == NULL, "no client created for rejected http://");
    CHECK(http_mock_init_calls() == 0, "esp_http_client_init not called for rejected http://");
#endif
}

// ------------------------------------------------------------- on_connected --

static void test_on_connected()
{
    std::printf("[TEST] 3. on_connected: логирует только https...\n");

    /*
     * Пункт 5 ревью П4. Раньше здесь стояли два CHECK(true, ...), которые не
     * могли упасть, а мутант m14 («логировать всегда») выживал. Теперь стенд
     * собирается с -DKOUKI_TEST_LOG_CAPTURE, и текст лога виден: https пишет
     * «https: server cert chain ok», http:// и NULL — молчат.
     */
    kouki_test_log_clear();
    kouki_http_on_connected("https://example.test");
    CHECK(kouki_test_log_count() == 1, "https: ровно одна строка лога");
    CHECK(kouki_test_log_contains("https: server cert chain ok"),
          "https: текст лога про цепочку сертификата");

    kouki_test_log_clear();
    kouki_http_on_connected("http://example.test");
    CHECK(kouki_test_log_count() == 0, "http:// не логирует https-ветку");

    kouki_test_log_clear();
    kouki_http_on_connected(NULL);
    CHECK(kouki_test_log_count() == 0, "NULL url не логирует");

    // Класс ведёт себя так же, как фасад (один экземпляр).
    kouki_test_log_clear();
    kouki::http().on_connected("https://example.test");
    CHECK(kouki_test_log_contains("https: server cert chain ok"),
          "метод класса логирует то же");

    // Колбэк ON_CONNECTED, установленный модулем, зовёт метод экземпляра и
    // тоже доходит до лога — то есть событие реально подключено.
    http_mock_reset();
    kouki_test_log_clear();
    esp_http_client_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://example.test/api/voice";
    esp_http_client_handle_t client = NULL;
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "client for event test");
    http_mock_fire_connected();
    CHECK(kouki_test_log_contains("https: server cert chain ok"),
          "ON_CONNECTED доводит url до on_connected и до лога");
    esp_http_client_cleanup(client);
}

// ------------------------------------------------------------ диагностика --

static void test_diagnose_tls()
{
    std::printf("[TEST] 4. diagnose_error: сбой проверки сертификата...\n");

    http_mock_reset();
    http_mock_set_tls_error(0x2700, 0);

    char buf[128] = {0};
    esp_http_client_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://example.test/api";
    esp_http_client_handle_t client = NULL;
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "client for tls diagnose");
    kouki_http_diagnose_error(client, cfg.url, ESP_FAIL, buf, sizeof(buf));
    CHECK(str_eq(buf, "Сертификат сервера не принят. Проверьте корневой CA."),
          "tls error code -> certificate message");
    CHECK(http_mock_tls_error_calls() == 1, "TLS error queried once for real client");
    esp_http_client_cleanup(client);

    // С флагом (а не кодом) — то же сообщение.
    http_mock_reset();
    http_mock_set_tls_error(0, 0x8000);
    std::memset(buf, 0, sizeof(buf));
    client = NULL;
    CHECK(kouki_http_new_client(&cfg, &client) == ESP_OK, "client for tls flags diagnose");
    kouki_http_diagnose_error(client, cfg.url, ESP_FAIL, buf, sizeof(buf));
    CHECK(str_eq(buf, "Сертификат сервера не принят. Проверьте корневой CA."),
          "tls flags -> certificate message");
    CHECK(http_mock_tls_error_calls() == 1, "TLS error queried once for real client");
    esp_http_client_cleanup(client);

    // Без handle TLS-ошибку спросить не у кого: остаётся обычная диагностика.
    http_mock_reset();
    http_mock_set_tls_error(0x2700, 0);
    std::memset(buf, 0, sizeof(buf));
    kouki_http_diagnose_error(NULL, "https://localhost:5300/api", ESP_FAIL, buf, sizeof(buf));
    CHECK(str_eq(buf, "Нет связи с сервером."), "NULL client -> no TLS query, connection message");
    CHECK(http_mock_tls_error_calls() == 0, "no TLS query without client handle");
}

static void test_diagnose_dns()
{
    std::printf("[TEST] 5. diagnose_error: сбой DNS...\n");

    char buf[128] = {0};

    // Код esp-tls «не удалось разрешить имя» — без похода в getaddrinfo.
    http_mock_reset();
    kouki_http_diagnose_error(NULL, "https://station.invalid/api",
                              ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME, buf, sizeof(buf));
    CHECK(str_eq(buf, "Имя сервера не найдено: station.invalid."),
          "CANNOT_RESOLVE_HOSTNAME -> dns message with host");

    // Реальный getaddrinfo: домен .invalid не резолвится никогда.
    http_mock_reset();
    std::memset(buf, 0, sizeof(buf));
    kouki_http_diagnose_error(NULL, "https://no-such-host.invalid:5300/api", ESP_FAIL, buf,
                              sizeof(buf));
    CHECK(str_eq(buf, "Имя сервера не найдено: no-such-host.invalid."),
          "unresolvable host -> dns message");

    // Хост без схемы и с портом разбирается так же.
    std::memset(buf, 0, sizeof(buf));
    kouki_http_diagnose_error(NULL, "no-such-host.invalid:5300/api", ESP_FAIL, buf, sizeof(buf));
    CHECK(str_eq(buf, "Имя сервера не найдено: no-such-host.invalid."),
          "host without scheme parsed");
}

static void test_diagnose_connection()
{
    std::printf("[TEST] 6. diagnose_error: нет связи, пустые аргументы...\n");

    char buf[128] = {0};

    // localhost резолвится, значит причина — связь.
    http_mock_reset();
    kouki_http_diagnose_error(NULL, "https://localhost:5300/api", ESP_FAIL, buf, sizeof(buf));
    CHECK(str_eq(buf, "Нет связи с сервером."), "resolvable host -> connection message");

    // Пустой URL: host пуст, getaddrinfo не зовём, остаётся «нет связи».
    std::memset(buf, 0, sizeof(buf));
    kouki_http_diagnose_error(NULL, NULL, ESP_FAIL, buf, sizeof(buf));
    CHECK(str_eq(buf, "Нет связи с сервером."), "NULL url -> connection message");

    // NULL-буфер и нулевая длина: тихий выход, TLS-ошибку не спрашиваем.
    http_mock_reset();
    kouki_http_diagnose_error(NULL, "https://example.test", ESP_FAIL, NULL, 0);
    CHECK(http_mock_tls_error_calls() == 0, "no TLS query for NULL buffer");
    kouki_http_diagnose_error(NULL, "https://example.test", ESP_FAIL, buf, 0);
    CHECK(http_mock_tls_error_calls() == 0, "no TLS query for zero length");

    /*
     * Пункт 4 ревью П4 (мутант m8): guard `err_buf == NULL || max_len == 0`.
     * Проверялся только max_len == 0. Кейс «NULL-буфер при max_len > 0» без
     * guard роняет процесс (snprintf(NULL, 128, ...)), то есть exit 139 —
     * именно это и наблюдал ревьюер. Здесь вызов обязан просто вернуться.
     */
    http_mock_reset();
    http_mock_set_tls_error(0x2700, 0);
    kouki_http_diagnose_error(NULL, "https://example.test", ESP_FAIL, NULL, 128);
    CHECK(true, "diagnose_error(NULL, max_len=128) вернулся без падения");
    /*
     * И самое важное: при NULL-буфере модуль даже не должен ходить в клиент
     * за TLS-ошибкой — guard стоит ДО всей диагностики. Без guard код упал бы
     * уже после этого запроса, поэтому проверять «0 вызовов» осмысленно
     * только вместе с отсутствием падения выше.
     */
    CHECK(http_mock_tls_error_calls() == 0,
          "при NULL-буфере TLS-ошибка даже не запрашивается");

    // Тот же guard для клиента с реальным handle.
    http_mock_reset();
    esp_http_client_config_t cfg2;
    std::memset(&cfg2, 0, sizeof(cfg2));
    cfg2.url = "https://example.test/api";
    esp_http_client_handle_t client2 = NULL;
    CHECK(kouki_http_new_client(&cfg2, &client2) == ESP_OK, "client for NULL-buffer case");
    http_mock_set_tls_error(0x2700, 0);
    kouki_http_diagnose_error(client2, cfg2.url, ESP_FAIL, NULL, 64);
    CHECK(http_mock_tls_error_calls() == 0,
          "с client handle, но NULL-буфером: TLS не запрашивается");
    esp_http_client_cleanup(client2);
}

// --------------------------------------------------- фасад и экземпляр ----

static void test_facade_and_instance()
{
    std::printf("[TEST] 7. Фасад и класс — один экземпляр...\n");

    CHECK(&kouki::http() == &kouki::http(), "http() returns the same instance");

    // Фасад и метод класса видят одну и ту же конфигурацию: после вызова через
    // фасад мок получает ту же конфигурацию, что и при вызове метода.
    http_mock_reset();
    esp_http_client_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.url = "https://example.test/api";
    esp_http_client_handle_t client = NULL;
    CHECK(kouki::http().new_client(&cfg, &client) == ESP_OK, "method new_client -> ESP_OK");
    CHECK(http_mock_init_calls() == 1, "method went through the same mock");
    esp_http_client_cleanup(client);
}

int main()
{
    std::printf("========================================\n");
    std::printf("   HOST CHECK: T121 Http (П4)           \n");
    std::printf("========================================\n");

    test_new_client_args();
    test_new_client_https();
    test_on_connected();
    test_diagnose_tls();
    test_diagnose_dns();
    test_diagnose_connection();
    test_facade_and_instance();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d/%d проверок успешно пройдено!\n", g_passed, g_checks);
    std::printf("========================================\n");
    return 0;
}
