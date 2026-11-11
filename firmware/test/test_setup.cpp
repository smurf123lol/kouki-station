// Host-стенд T121 П11: класс kouki::Setup и его C-фасад.
//
// SoftAP и httpd на хосте не поднимаются: support/setup/ подменяет
// esp_http_server, esp_mac, USB-Serial-JTAG, задачи FreeRTOS и heap, а UI,
// голосовой автомат и историю — счётчики support/setup/setup_stubs.c.
// Проверяется прежнее поведение C-модуля 1:1: имя SoftAP из MAC, запрет
// повторного входа в портал, регистрация трёх маршрутов, валидации формы
// /save, разбор консольных команд и подъём консоли через USB-Serial-JTAG.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app_config.h"
#include "app_history.h"
#include "app_setup.h"
#include "app_setup.hpp"
#include "app_voice.h"
#include "app_wifi.hpp"
#include "log_capture.h"
#include "nvs_mock.h"
#include "wifi_mock.h"
#include "setup_mock.h"
#include "setup_stubs.h"

// Стражи конфига стенда. Стенд Setup собирается с одним набором CONFIG_KOUKI_*
// (support/setup/sdkconfig.h через -include, см. tools/test-host.sh). Если набор
// сузится — уберут define, снимут -include или поставят флаг в 0, — проверки
// test_seed_history/test_mock_done молча выпадут и прогон останется зелёным
// (220 → 214 проверок, exit 0). Именно так умерли 4 проверки до пункта 32 ревью
// П11, поэтому регрессия конфига должна валить СБОРКУ, а не уменьшать счётчик.
#if !defined(CONFIG_KOUKI_TEST_CONSOLE_CMDS) || (CONFIG_KOUKI_TEST_CONSOLE_CMDS + 0) != 1
#error "test_setup.cpp должен видеть CONFIG_KOUKI_TEST_CONSOLE_CMDS=1 из support/setup/sdkconfig.h: иначе блоки test_seed_history/test_mock_done молча выпадают (см. пункт 32 ревью П11 и README стендов)."
#endif
// Тот же класс дефекта для второго TU: app_setup.cpp печатает справку и
// регистрирует консольные команды под этим флагом, поэтому значение сверяется
// ещё и здесь — иначе «1» и «0» в help неразличимы (мутация C).
#if !defined(CONFIG_KOUKI_WIFI_TIMEOUT_S) || (CONFIG_KOUKI_WIFI_TIMEOUT_S + 0) != 25
#error "test_setup.cpp должен видеть CONFIG_KOUKI_WIFI_TIMEOUT_S=25 (Kconfig.projbuild): разные наборы sdkconfig у app_setup.cpp и test_setup.cpp = разные представления о сборке."
#endif

static int g_checks = 0;

#define CHECK(cond, msg) do {                                                 \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        std::exit(1);                                                         \
    }                                                                         \
} while (0)

namespace {

const char *const kNs = KOUKI_CONFIG_NVS_NAMESPACE;

/* MAC мока: 24:6F:28:A1:BC:5E -> имя сети «Kouki-Setup-BC5E». */
const char *const kApSsid = "Kouki-Setup-BC5E";
const char *const kApUrl = "http://192.168.4.1";

const char *const kPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBszCCAVmgAwIBAgIUJ+6wS2e0h1g1h1g1h1g1h1g1h1g1h1g1h1g1h1g1h1g1\n"
    "-----END CERTIFICATE-----\n";

/**
 * Сброс состояния стенда перед каждым тестом.
 *
 * Экземпляр Setup один на процесс: m_active/m_server остаются от прошлого
 * теста, а Config — один на процесс по построению (П2). Поэтому сначала
 * чистим «флеш» и заново инициализируем Config полной конфигурацией, потом
 * сбрасываем моки: портал начинается с известного чистого состояния.
 */
void reset_all()
{
    setup_mocks_reset();
    setup_stubs_reset();

    nvs_mock_reset();
    nvs_mock_seed_u8(kNs, "cfg_ver", KOUKI_CONFIG_VERSION);
    nvs_mock_seed_str(kNs, "wifi_ssid", "kouki-home");
    nvs_mock_seed_str(kNs, "wifi_pass", "secret-pass");
    nvs_mock_seed_str(kNs, "srv_url", "http://192.168.1.20:5300");
    nvs_mock_seed_str(kNs, "srv_token", "0123456789abcdef");
    nvs_mock_seed_str(kNs, "dev_source", "station");
    nvs_mock_seed_u32(kNs, "ota_int_s", 43200);
    nvs_mock_seed_u8(kNs, "silent", 0);
    nvs_mock_seed_u8(kNs, "wake_en", 1);
    nvs_mock_seed_u8(kNs, "wake_th", 65);
    CHECK(kouki_config_init() == ESP_OK, "init() конфигурации -> ESP_OK");
}

/** Запрос к обработчику портала: тело формы и user_ctx как у httpd. */
httpd_req_t make_req(const char *body)
{
    httpd_req_t req = {};
    req.content_len = body != NULL ? (int)std::strlen(body) : 0;
    req.user_ctx = nullptr;
    req.body = body;
    return req;
}

/** Прямой вызов обработчика маршрута по индексу (как его позвал бы httpd). */
esp_err_t call_route(size_t index, const char *body)
{
    httpd_req_t req = make_req(body);
    httpd_uri_t uri = {};
    // Таблица маршрутов принадлежит экземпляру: берём её у него.
    const httpd_uri_t *routes = kouki::setup().routes();
    uri = routes[index];
    // httpd подставляет user_ctx маршрута в запрос — стенд делает то же,
    // иначе trampoline не найдёт экземпляр.
    req.user_ctx = uri.user_ctx;
    return uri.handler(&req);
}

/** Исходник модуля из firmware/main (KOUKI_MAIN_DIR ставит test-host.sh). */
std::string read_source(const char *name)
{
    const std::string path = std::string(KOUKI_MAIN_DIR) + "/" + name;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "FAIL: не открылся %s\n", path.c_str());
        std::exit(1);
    }
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    std::fclose(f);
    return out;
}

/**
 * Тело функции по сигнатуре: от первого '{' после неё до парной '}'.
 *
 * Нужно для текстов printf (help, diag, баннер консоли): на хосте они идут
 * в stdout стенда, а не в перехват логов, поэтому сверяются по исходнику —
 * так же, как это делает стенд Station для ESP_LOG.
 */
std::string body_of(const std::string &src, const std::string &signature)
{
    const size_t at = src.find(signature);
    if (at == std::string::npos) {
        std::fprintf(stderr, "FAIL: в исходнике нет сигнатуры «%s»\n", signature.c_str());
        std::exit(1);
    }
    size_t open = src.find('{', at + signature.size());
    if (open == std::string::npos) {
        std::fprintf(stderr, "FAIL: нет тела у «%s»\n", signature.c_str());
        std::exit(1);
    }
    int depth = 0;
    for (size_t i = open; i < src.size(); i++) {
        if (src[i] == '{') {
            depth++;
        } else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                return src.substr(open, i - open + 1);
            }
        }
    }
    std::fprintf(stderr, "FAIL: тело «%s» не закрылось\n", signature.c_str());
    std::exit(1);
}

/** Форма /save из готовых значений (все поля, которые читает модуль). */
std::string form(const std::string &ssid, const std::string &url, const std::string &token,
                 const std::string &pass = "secret-pass", const std::string &ca = "",
                 const std::string &wake_en = "1", const std::string &wake_th = "65")
{
    std::string body;
    body += "wifi_ssid=" + ssid;
    body += "&wifi_pass=" + pass;
    body += "&srv_url=" + url;
    body += "&srv_token=" + token;
    body += "&dev_source=station";
    if (!ca.empty()) {
        body += "&ca_pem=" + ca;
    }
    body += "&wake_en=" + wake_en;
    body += "&wake_th=" + wake_th;
    return body;
}

}  // namespace

// ------------------------------------------------------------------ тесты --

static void test_routes()
{
    std::printf("[TEST] 1. Таблица маршрутов портала: uri/method как в master...\n");

    CHECK(kouki::Setup::route_count() == 3, "маршрутов ровно три");
    const httpd_uri_t *routes = kouki::setup().routes();
    CHECK(routes != nullptr, "таблица маршрутов доступна");

    CHECK(std::strcmp(routes[0].uri, "/") == 0, "первый маршрут — /");
    CHECK(routes[0].method == HTTP_GET, "…метод GET");
    CHECK(std::strcmp(routes[1].uri, "/index.html") == 0, "второй маршрут — /index.html");
    CHECK(routes[1].method == HTTP_GET, "…метод GET");
    CHECK(std::strcmp(routes[2].uri, "/save") == 0, "третий маршрут — /save");
    CHECK(routes[2].method == HTTP_POST, "…метод POST");

    // user_ctx каждого маршрута — сам экземпляр: иначе httpd позовёт не тот.
    const void *self = static_cast<const void *>(&kouki::setup());
    CHECK(routes[0].user_ctx == self, "user_ctx / -> экземпляр");
    CHECK(routes[1].user_ctx == self, "user_ctx /index.html -> экземпляр");
    CHECK(routes[2].user_ctx == self, "user_ctx /save -> экземпляр");

    // Оба GET-маршрута ведут на один обработчик (как в master).
    CHECK(routes[0].handler == routes[1].handler, "/ и /index.html — один обработчик");
    CHECK(routes[2].handler != routes[0].handler, "/save — отдельный обработчик");
}

static void test_start()
{
    std::printf("[TEST] 2. Запуск портала: SoftAP из MAC, три маршрута, лог...\n");

    reset_all();
    CHECK(!kouki::setup().is_active(), "до старта портал неактивен");
    CHECK(!kouki_setup_is_active(), "фасад: до старта портал неактивен");

    CHECK(kouki_setup_start() == ESP_OK, "start() -> ESP_OK");
    CHECK(kouki::setup().is_active(), "после старта портал активен");
    CHECK(kouki_setup_is_active(), "фасад видит активность");

    // Имя сети собирается из MAC: «Kouki-Setup-» + байты 4 и 5.
    CHECK(std::strcmp(wifi_mock_ap_ssid(), kApSsid) == 0, "SSID SoftAP из MAC");
    CHECK(wifi_mock_ap_channel() == 1, "канал 1");
    CHECK(wifi_mock_ap_authmode() == WIFI_AUTH_OPEN, "открытая сеть");
    CHECK(wifi_mock_ap_max_connection() == 4, "до 4 клиентов");
    CHECK(wifi_mock_mode() == WIFI_MODE_AP, "режим — точка доступа");
    CHECK(wifi_mock_create_ap_calls() == 1, "создан netif точки доступа");
    CHECK(setup_ui_setup_calls() == 1, "UI показал экран настройки один раз");
    CHECK(std::strcmp(setup_ui_setup_ssid(), kApSsid) == 0, "UI получил тот же SSID");
    CHECK(std::strcmp(setup_ui_setup_url(), kApUrl) == 0, "UI получил адрес портала");

    CHECK(setup_httpd_start_calls() == 1, "httpd_start вызван один раз");
    CHECK(setup_httpd_register_calls() == 3, "зарегистрированы три маршрута");
    CHECK(std::strcmp(setup_httpd_route_uri(0), "/") == 0, "первый зарегистрированный — /");
    CHECK(std::strcmp(setup_httpd_route_uri(2), "/save") == 0, "третий зарегистрированный — /save");

    // Повторный вход в портал: master не проверяет m_server и зовёт httpd_start
    // заново (защиты нет — поведение сохранено 1:1, а не «улучшено»).
    CHECK(kouki_setup_start() == ESP_OK, "повторный start() -> ESP_OK");
    CHECK(setup_httpd_start_calls() == 2, "повторный start() снова зовёт httpd_start (как в master)");
    CHECK(setup_ui_setup_calls() == 2, "UI обновляется и при повторном входе");
}

static void test_start_httpd_failure()
{
    std::printf("[TEST] 3. Отказ httpd_start: портал активен, start() отдаёт ошибку...\n");

    reset_all();
    setup_httpd_fail_next_start(ESP_FAIL);

    const esp_err_t err = kouki_setup_start();
    CHECK(err == ESP_FAIL, "start() пробрасывает ошибку httpd_start");
    CHECK(kouki::setup().is_active(), "портал всё равно помечен активным (как в master)");
    CHECK(setup_httpd_register_calls() == 0, "маршруты не регистрируются при отказе");
}

static void test_index_get()
{
    std::printf("[TEST] 4. Страница настройки: HTML, тип ответа...\n");

    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    CHECK(call_route(0, nullptr) == ESP_OK, "GET / -> ESP_OK");

    CHECK(std::strcmp(setup_httpd_last_type(), "text/html; charset=utf-8") == 0,
          "тип ответа — HTML в utf-8");
    const char *html = setup_httpd_last_body();
    CHECK(std::strstr(html, "K O U K I") != nullptr, "страница — портал Kouki");
    CHECK(std::strstr(html, "name=\"wifi_ssid\"") != nullptr, "есть поле SSID");
    CHECK(std::strstr(html, "name=\"srv_url\"") != nullptr, "есть поле URL сервера");
    CHECK(std::strstr(html, "name=\"srv_token\"") != nullptr, "есть поле токена");
    CHECK(std::strstr(html, "action=\"/save\"") != nullptr, "форма шлёт POST /save");
}

static void test_save_ok()
{
    std::printf("[TEST] 5. Сохранение формы: настройки в NVS, ответ и перезагрузка...\n");

    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    const std::string body = form("new-home", "https://kouki.local", "new-token-123");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "POST /save -> ESP_OK");

    CHECK(std::strcmp(kouki_config_get_wifi_ssid(), "new-home") == 0, "SSID из формы применён");
    CHECK(std::strcmp(kouki_config_get_wifi_pass(), "secret-pass") == 0, "пароль из формы применён");
    CHECK(std::strcmp(kouki_config_get_srv_url(), "https://kouki.local") == 0, "URL из формы применён");
    CHECK(std::strcmp(kouki_config_get_srv_token(), "new-token-123") == 0, "токен из формы применён");

    // Значения дошли до «флеша»: Config::save() записал их в NVS.
    const char *stored = nullptr;
    CHECK(nvs_mock_get_str(kNs, "wifi_ssid", &stored) && std::strcmp(stored, "new-home") == 0,
          "SSID записан в NVS");
    CHECK(nvs_mock_get_str(kNs, "srv_url", &stored) && std::strcmp(stored, "https://kouki.local") == 0,
          "URL записан в NVS");

    CHECK(std::strstr(setup_httpd_last_body(), "Сохранено. Перезагрузка") != nullptr,
          "ответ портала — «Сохранено. Перезагрузка…»");
    CHECK(std::strcmp(setup_httpd_last_type(), "text/html; charset=utf-8") == 0,
          "ответ — HTML в utf-8");

    // Перезагрузка отложена задачей, а не сделана в обработчике.
    CHECK(setup_task_creates() == 1, "создана задача отложенной перезагрузки");
    CHECK(std::strcmp(setup_task_last_name(), "restart") == 0, "имя задачи — restart");
    CHECK(setup_task_last_stack() == 2048, "стек задачи — 2048");
    CHECK(setup_task_last_priority() == 5, "приоритет — 5");
    CHECK(setup_restart_calls() == 0, "перезагрузка ещё не случилась");
    CHECK(setup_task_run_last(), "задача перезагрузки запускается");
    CHECK(setup_task_delay_ticks() == pdMS_TO_TICKS(2000), "пауза перед перезагрузкой — 2 с");
    CHECK(setup_restart_calls() == 1, "после задачи плата перезагружается");
}

static void test_save_validation()
{
    std::printf("[TEST] 6. Валидация формы: SSID, URL, токен, сертификат...\n");

    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");

    // Пустой SSID.
    std::string body = form("", "https://kouki.local", "new-token-123");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "пустой SSID: обработчик отвечает");
    CHECK(setup_httpd_last_status() == 400, "пустой SSID -> 400");
    CHECK(std::strstr(setup_httpd_last_body(), "SSID не может быть пустым") != nullptr,
          "текст отказа про SSID");

    // URL без схемы.
    body = form("home", "kouki.local", "new-token-123");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "URL без схемы: обработчик отвечает");
    CHECK(setup_httpd_last_status() == 400, "URL без схемы -> 400");
    CHECK(std::strstr(setup_httpd_last_body(), "URL должен начинаться") != nullptr,
          "текст отказа про URL");

    // Короткий токен.
    body = form("home", "https://kouki.local", "short");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "короткий токен: обработчик отвечает");
    CHECK(setup_httpd_last_status() == 400, "короткий токен -> 400");
    CHECK(std::strstr(setup_httpd_last_body(), "Токен слишком короткий") != nullptr,
          "текст отказа про токен");

    // PEM без маркеров сертификата.
    body = form("home", "https://kouki.local", "new-token-123", "secret-pass", "not-a-pem");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "битый PEM: обработчик отвечает");
    CHECK(setup_httpd_last_status() == 400, "битый PEM -> 400");
    CHECK(std::strstr(setup_httpd_last_body(), "ожидался PEM") != nullptr,
          "текст отказа про PEM");

    // После всех отказов настройки в NVS не поменялись.
    const char *stored = nullptr;
    CHECK(nvs_mock_get_str(kNs, "wifi_ssid", &stored) && std::strcmp(stored, "kouki-home") == 0,
          "отказы валидации не тронули NVS");
    CHECK(setup_task_creates() == 0, "перезагрузка не планировалась");
}

static void test_save_ca_pem_and_wake()
{
    std::printf("[TEST] 7. Сохранение с CA-PEM и параметрами wake-word...\n");

    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");

    std::string body = form("home", "https://kouki.local", "new-token-123", "secret-pass", kPem, "1", "80");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "POST /save с PEM -> ESP_OK");
    CHECK(setup_httpd_last_status() == 0, "успех не ставит HTTP-код ошибки");

    const char *stored = nullptr;
    CHECK(nvs_mock_get_str(kNs, "ca_pem", &stored) && std::strstr(stored, "BEGIN CERTIFICATE") != nullptr,
          "CA-PEM записан в NVS");
    CHECK(kouki_config_get_wake_enabled(), "wake_en=1 из формы -> включён");
    CHECK(kouki_config_get_wake_threshold() == 80, "порог wake из формы — 80");

    // wake_en=0 выключает детектор; порог вне диапазона игнорируется.
    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят заново");
    body = form("home", "https://kouki.local", "new-token-123", "secret-pass", "", "0", "7");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "POST /save с wake_en=0 -> ESP_OK");
    CHECK(!kouki_config_get_wake_enabled(), "wake_en=0 из формы -> выключен");
    CHECK(kouki_config_get_wake_threshold() == 65, "порог вне диапазона не применяется");
}

static void test_save_too_large()
{
    std::printf("[TEST] 8. Слишком большое тело формы: 400 без чтения...\n");

    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");

    const httpd_uri_t *routes = kouki::setup().routes();
    httpd_req_t req = {};
    req.content_len = 8193;  // предел модуля — 8192
    req.body = "";
    req.user_ctx = routes[2].user_ctx;
    CHECK(routes[2].handler(&req) == ESP_FAIL, "тело > 8192 -> ESP_FAIL");
    CHECK(setup_httpd_send_err_calls() == 1, "отправлен ответ об ошибке");
    CHECK(setup_httpd_last_err_status() == 400, "код ответа — 400");

    // Пустое тело — тоже отказ.
    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    req.content_len = 0;
    CHECK(routes[2].handler(&req) == ESP_FAIL, "пустое тело -> ESP_FAIL");
    CHECK(setup_httpd_last_err_status() == 400, "пустое тело -> 400");
}

static void test_console_commands()
{
    std::printf("[TEST] 9. Консоль: set_url/set_token/set_wifi/set_silent/set_wake...\n");

    reset_all();
    kouki::setup().handle_console_command("set_url https://new.example");
    CHECK(std::strcmp(kouki_config_get_srv_url(), "https://new.example") == 0,
          "set_url применяет URL");

    kouki::setup().handle_console_command("set_token 0123456789abcdef");
    CHECK(std::strcmp(kouki_config_get_srv_token(), "0123456789abcdef") == 0,
          "set_token применяет токен");

    kouki::setup().handle_console_command("set_wifi newssid newpass");
    CHECK(std::strcmp(kouki_config_get_wifi_ssid(), "newssid") == 0, "set_wifi меняет SSID");
    CHECK(std::strcmp(kouki_config_get_wifi_pass(), "newpass") == 0, "set_wifi меняет пароль");

    kouki::setup().handle_console_command("set_wifi openssid");
    CHECK(std::strcmp(kouki_config_get_wifi_ssid(), "openssid") == 0, "set_wifi без пароля");
    CHECK(std::strcmp(kouki_config_get_wifi_pass(), "") == 0, "пароль сброшен в пустой");

    kouki::setup().handle_console_command("set_silent 1");
    CHECK(kouki_config_is_silent(), "set_silent 1 включает тишину");
    kouki::setup().handle_console_command("set_silent 0");
    CHECK(!kouki_config_is_silent(), "set_silent 0 выключает тишину");

    kouki::setup().handle_console_command("set_wake 1");
    CHECK(kouki_config_get_wake_enabled(), "set_wake 1 включает детектор");
    kouki::setup().handle_console_command("set_wake 0");
    CHECK(!kouki_config_get_wake_enabled(), "set_wake 0 выключает детектор");

    kouki::setup().handle_console_command("set_wake_th 55");
    CHECK(kouki_config_get_wake_threshold() == 55, "set_wake_th в диапазоне применяется");
    kouki::setup().handle_console_command("set_wake_th 5");
    CHECK(kouki_config_get_wake_threshold() == 55, "set_wake_th вне диапазона игнорируется");
    kouki::setup().handle_console_command("set_wake_th 99");
    CHECK(kouki_config_get_wake_threshold() == 55, "порог выше 95 не применяется");

    kouki::setup().handle_console_command("set_source field-1");
    CHECK(std::strcmp(kouki_config_get_dev_source(), "field-1") == 0, "set_source меняет метку");
}

static void test_console_validation()
{
    std::printf("[TEST] 10. Консоль: отказ при неверных аргументах...\n");

    reset_all();
    const char *before_url = kouki_config_get_srv_url();

    kouki::setup().handle_console_command("set_url kouki.local");
    CHECK(std::strcmp(kouki_config_get_srv_url(), before_url) == 0,
          "set_url без схемы не меняет URL");

    kouki::setup().handle_console_command("set_token short");
    CHECK(std::strcmp(kouki_config_get_srv_token(), "0123456789abcdef") == 0,
          "короткий токен не применяется");

    kouki::setup().handle_console_command("set_wifi ");
    CHECK(std::strcmp(kouki_config_get_wifi_ssid(), "kouki-home") == 0,
          "пустой SSID не применяется");

    kouki::setup().handle_console_command("unknown_command");
    CHECK(std::strcmp(kouki_config_get_wifi_ssid(), "kouki-home") == 0,
          "неизвестная команда ничего не меняет");
}

static void test_console_save_reset()
{
    std::printf("[TEST] 11. Консоль: save/reset/show/state/diag...\n");

    reset_all();
    kouki::setup().handle_console_command("set_url https://saved.example");
    kouki::setup().handle_console_command("save");

    const char *stored = nullptr;
    CHECK(nvs_mock_get_str(kNs, "srv_url", &stored) &&
              std::strcmp(stored, "https://saved.example") == 0,
          "save записывает URL в NVS");

    kouki::setup().handle_console_command("reset");
    CHECK(nvs_mock_has_key(kNs, "srv_url") == false, "reset стирает ключи из NVS");
    CHECK(kouki_config_get_srv_url()[0] == '\0', "после reset URL сброшен в пустой (как set_defaults)");

    // show и state выводят состояние автомата, но ничего не ломают.
    kouki::setup().handle_console_command("show");
    CHECK(setup_voice_print_state_calls() == 1, "show печатает состояние FSM");
    kouki::setup().handle_console_command("state");
    CHECK(setup_voice_print_state_calls() == 2, "state печатает состояние FSM");

    kouki::setup().handle_console_command("diag");
    kouki::setup().handle_console_command("help");
}

static void test_console_voice()
{
    std::printf("[TEST] 12. Консоль: record/stop/ask и тестовые команды...\n");

    reset_all();
    kouki::setup().handle_console_command("record");
    CHECK(setup_voice_start_calls() == 1, "record запускает запись");
    CHECK(setup_voice_last_session() == KOUKI_SESSION_TASK, "record — сессия TASK");

    kouki::setup().handle_console_command("stop");
    CHECK(setup_voice_stop_calls() == 1, "stop отправляет на сервер");

    kouki::setup().handle_console_command("ask");
    CHECK(setup_voice_start_calls() == 2, "ask запускает запись");
    CHECK(setup_voice_last_session() == KOUKI_SESSION_ASK, "ask — сессия ASK");

    // Без #if: видимость и значение флага проверены стражами выше, поэтому
    // эти проверки обязаны исполняться всегда (иначе счётчик молча падает).
    kouki::setup().handle_console_command("test_seed_history");
    CHECK(setup_history_clear_calls() == 1, "test_seed_history чистит историю");
    CHECK(setup_history_push_calls() == KOUKI_HISTORY_CAPACITY,
          "test_seed_history добавляет KOUKI_HISTORY_CAPACITY записей");
    CHECK(kouki_history_count() == KOUKI_HISTORY_CAPACITY, "история заполнена");

    kouki::setup().handle_console_command("test_mock_done");
    CHECK(setup_voice_mock_done_calls() == 1, "test_mock_done доходит до FSM");
}

static void test_console_init()
{
    std::printf("[TEST] 13. Консоль USB-Serial-JTAG: драйвер, VFS, задача...\n");

    reset_all();
    CHECK(kouki_setup_console_init() == ESP_OK, "console_init() -> ESP_OK");
    CHECK(setup_jtag_install_calls() == 1, "драйвер USB-Serial-JTAG установлен");
    CHECK(setup_jtag_vfs_calls() == 1, "VFS переключён на драйвер");
    CHECK(setup_jtag_rx_calls() == 1, "переводы строк RX настроены");
    CHECK(setup_jtag_tx_calls() == 1, "переводы строк TX настроены");
    CHECK(setup_task_creates() == 1, "создана задача консоли");
    CHECK(std::strcmp(setup_task_last_name(), "kouki.console") == 0, "имя задачи — kouki.console");
    CHECK(setup_task_last_stack() == 6144, "стек консоли — 6144");
    CHECK(setup_task_last_priority() == 3, "приоритет консоли — 3");

    // Отказ установки драйвера не мешает поднять задачу (как в master:
    // VFS настраивается только при успехе, xTaskCreate — всегда).
    reset_all();
    setup_jtag_fail_next_install(ESP_FAIL);
    CHECK(kouki_setup_console_init() == ESP_OK, "console_init() при отказе драйвера -> ESP_OK");
    CHECK(setup_jtag_install_calls() == 1, "попытка установки была");
    CHECK(setup_jtag_vfs_calls() == 0, "VFS не переключался");
    CHECK(setup_task_creates() == 1, "задача консоли всё равно создана");

    // Отказ xTaskCreate -> ESP_FAIL.
    reset_all();
    setup_task_fail_next_create();
    CHECK(kouki_setup_console_init() == ESP_FAIL, "отказ xTaskCreate -> ESP_FAIL");
}

// Пункт 31 ревью П11: ветки, которые стенд не видел.
static void test_review_gaps()
{
    std::printf("[TEST] 15. Ветки ревью П11: PEM-разбор, отказ save(), dev_source, help...\n");

    // (a) PEM с маркерами, но мусорным телом: маркерный guard пропускает,
    //     ловит разбор mbedtls. Без этой проверки снятие второго guard-а
    //     выживало (первый отвергал «not-a-pem» и тест был доволен).
    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    std::string body = form("home", "https://kouki.local", "new-token-123", "secret-pass",
                            "-----BEGIN CERTIFICATE-----@@@@-----END CERTIFICATE-----");
    CHECK(call_route(2, body.c_str()) == ESP_OK, "PEM с битым телом: обработчик отвечает");
    CHECK(setup_httpd_last_status() == 400, "PEM с битым телом -> 400");
    CHECK(std::strstr(setup_httpd_last_body(), "не разбирается") != nullptr,
          "текст отказа — «Сертификат CA не разбирается.»");
    CHECK(setup_task_creates() == 0, "битый PEM не планирует перезагрузку");

    // (b) save() вернул ошибку -> 500 «Ошибка записи в NVS», ESP_FAIL,
    //     перезагрузки нет. Раньше ветка была не покрыта: мок NVS не умел
    //     отказывать на записи.
    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    nvs_mock_fail_next_commit(ESP_ERR_NVS_READ_ONLY);
    body = form("home", "https://kouki.local", "new-token-123");
    CHECK(call_route(2, body.c_str()) == ESP_FAIL, "отказ save() -> ESP_FAIL");
    CHECK(setup_httpd_send_err_calls() == 1, "отправлен ответ об ошибке");
    CHECK(setup_httpd_last_err_status() == 500, "отказ save() -> 500");
    CHECK(setup_task_creates() == 0, "отказ save() не планирует перезагрузку");

    // (c) пустой dev_source в форме -> метка по умолчанию «station».
    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    body = "wifi_ssid=home&wifi_pass=secret-pass&srv_url=https://kouki.local"
           "&srv_token=new-token-123&dev_source=&wake_en=1&wake_th=65";
    CHECK(call_route(2, body.c_str()) == ESP_OK, "пустой dev_source: обработчик отвечает");
    CHECK(std::strcmp(kouki_config_get_dev_source(), KOUKI_CONFIG_DEFAULT_SOURCE) == 0,
          "пустой dev_source -> KOUKI_CONFIG_DEFAULT_SOURCE");
    CHECK(std::strcmp(KOUKI_CONFIG_DEFAULT_SOURCE, "station") == 0, "метка по умолчанию — station");

    // (d) отказ esp_wifi_set_config(AP) пробрасывается и httpd не поднимается.
    reset_all();
    wifi_mock_fail_next_set_config(ESP_ERR_INVALID_ARG);
    CHECK(kouki_setup_start() == ESP_ERR_INVALID_ARG, "отказ set_config(AP) -> тот же код");
    CHECK(setup_httpd_start_calls() == 0, "при отказе set_config httpd не поднимался");

    // (e) стек веб-сервера: конфигурация httpd_start видна моку.
    reset_all();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    CHECK(setup_httpd_last_config()->port == 80, "порт по умолчанию — 80");
    CHECK(setup_httpd_last_config()->stack_size == 6144, "стек веб-сервера — 6144");

    // (f) тексты логов портала (перехват ESP_LOG в стенде).
    reset_all();
    kouki_test_log_clear();
    CHECK(kouki_setup_start() == ESP_OK, "портал поднят");
    CHECK(kouki_test_log_contains("SoftAP запущен: Kouki-Setup-BC5E (IP 192.168.4.1)"),
          "лог SoftAP с именем сети и адресом");
    CHECK(kouki_test_log_contains("веб-сервер настройки поднят на порту 80"),
          "лог поднятого веб-сервера");

    reset_all();
    kouki_test_log_clear();
    setup_httpd_fail_next_start(ESP_FAIL);
    CHECK(kouki_setup_start() == ESP_FAIL, "отказ httpd_start -> ESP_FAIL");
    CHECK(kouki_test_log_contains("не удалось запустить веб-сервер: ESP_FAIL"),
          "лог отказа веб-сервера");

    // (g) help/diag/баннер консоли — printf, а не ESP_LOG: сверяем исходник.
    const std::string src = read_source("app_setup.cpp");
    const std::string help = body_of(src, "strcmp(cmd, \"help\") == 0");
    CHECK(help.find("Команды консоли:") != std::string::npos, "help печатает заголовок");
    // Сверяются строки целиком (команда + описание: подмена только описания
    // раньше выживала, потому что проверялось лишь имя команды).
    for (const char *line : {
             "  set_wifi <ssid> [pass]  — задать Wi-Fi (SSID и опциональный пароль)\\n",
             "  set_token <token>       — задать bearer-токен сервера (мин. 8 символов)\\n",
             "  set_url <url>           — задать URL сервера (http:// или https://)\\n",
             "  set_source <source>     — задать метку источника (по умолчанию 'station')\\n",
             "  set_silent <0|1>        — включить/выключить режим тишины (без голоса)\\n",
             "  set_wake <0|1>          — включить/выключить wake-word «эй гин»\\n",
             "  set_wake_th <10..95>    — порог срабатывания wake-word (по умолч. 75)\\n",
             "  show                    — показать текущие настройки (секреты скрыты ***)\\n",
             "  state                   — показать статус голосового автомата (FSM)\\n",
             "  save                    — сохранить настройки в NVS\\n",
             "  reset                   — очистить настройки в NVS\\n",
             "  record                  — начать голосовую запись (сессия TASK)\\n",
             "  stop                    — остановить запись и отправить на сервер\\n",
             "  ask                     — начать голосовую запись (сессия ASK)\\n",
             "  diag                    — показать свободную память (heap/PSRAM)\\n",
             "  reboot                  — перезагрузить станцию\\n",
             "  help                    — эта справка\\n\\n",
         }) {
        CHECK(help.find(line) != std::string::npos,
              "help перечисляет команду вместе с описанием");
    }
    CHECK(help.find("test_seed_history") != std::string::npos, "help знает test_seed_history");
    CHECK(help.find("test_mock_done") != std::string::npos, "help знает test_mock_done");

    const std::string diag = body_of(src, "strcmp(cmd, \"diag\") == 0");
    CHECK(diag.find("Свободная память кучи: %u байт, PSRAM: %u байт") != std::string::npos,
          "diag печатает heap и PSRAM с байтами");

    const std::string loop = body_of(src, "void Setup::console_loop()");
    CHECK(loop.find("=== Kouki Station Console ===") != std::string::npos,
          "баннер консоли на месте");
    CHECK(loop.find("Введите 'help' для списка команд.") != std::string::npos,
          "подсказка про help на месте");
    CHECK(loop.find("нет памяти под буфер консоли") != std::string::npos,
          "лог отказа malloc под строку консоли");

    // (h) Граница строки консоли: kConsoleLineMax участвует в двух местах
    // (malloc и fgets) — уменьшение константы на хосте ненаблюдаемо (ввод
    // идёт из stdin), поэтому проверяется и значение, и обе точки.
    CHECK(src.find("constexpr size_t kConsoleLineMax = 512;") != std::string::npos,
          "граница строки консоли — 512 байт");
    CHECK(loop.find("malloc(kConsoleLineMax)") != std::string::npos,
          "буфер консоли выделяется по kConsoleLineMax");
    CHECK(loop.find("fgets(line, kConsoleLineMax, stdin)") != std::string::npos,
          "fgets читает не больше kConsoleLineMax");
}

static void test_facade()
{
    std::printf("[TEST] 14. C-фасад 1:1: те же объекты и состояние...\n");

    reset_all();
    CHECK(kouki_setup_is_active() == kouki::setup().is_active(),
          "фасад и класс видят одно состояние");
    CHECK(kouki_setup_start() == ESP_OK, "фасад: старт портала");
    CHECK(kouki_setup_is_active(), "фасад: портал активен после старта");
    CHECK(kouki::setup().is_active(), "класс видит тот же старт");
}

int main()
{
    std::printf("=== Setup: host-стенд (T121 П11) ===\n");
    test_routes();
    test_start();
    test_start_httpd_failure();
    test_index_get();
    test_save_ok();
    test_save_validation();
    test_save_ca_pem_and_wake();
    test_save_too_large();
    test_console_commands();
    test_console_validation();
    test_console_save_reset();
    test_console_voice();
    test_console_init();
    test_review_gaps();
    test_facade();
    std::printf("=== Setup: %d проверок PASS ===\n", g_checks);
    return 0;
}
