// Host-стенд T121 П7: класс kouki::Wifi и его C-фасад.
//
// Сеть не поднимается: esp_wifi/esp_netif/esp_event/esp_timer заменены моком
// (support/wifi/), который запоминает вызовы и «доставляет» события сам или
// по команде теста. Проверяется прежнее поведение C-модуля 1:1: порядок
// подъёма стека, SSID/пароль из Config, backoff повторов 1→2→4→8→16 с,
// сброс backoff при получении адреса и kouki_wifi_get_info.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "app_wifi.h"
#include "app_wifi.hpp"
#include "nvs_mock.h"
#include "wifi_mock.h"

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

/* Адрес и шлюз, которые «выдаёт» сеть: 192.168.1.42 / 192.168.1.1.
 * Как и в IDF, адрес хранится в сетевом порядке: первый октет — младший байт. */
constexpr uint32_t kIp = (192u) | (168u << 8) | (1u << 16) | (42u << 24);
constexpr uint32_t kGw = (192u) | (168u << 8) | (1u << 16) | (1u << 24);

/** Полная валидная конфигурация в «флеше» + перезагрузка Config из NVS. */
void seed_complete_config()
{
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
    CHECK(kouki_config_is_complete(), "заложенная конфигурация полная");
}

}  // namespace

// ------------------------------------------------------------------ тесты --

static void test_incomplete_config()
{
    std::printf("[TEST] 1. Неполная конфигурация: отказ до старта станции...\n");

    // Пустой «флеш»: Config остаётся с дефолтами, is_complete() = false.
    nvs_mock_reset();
    CHECK(kouki_config_init() == ESP_OK, "init() на пустом NVS -> ESP_OK");
    CHECK(!kouki_config_is_complete(), "пустая конфигурация неполная");

    wifi_mock_reset();
    CHECK(kouki::wifi().connect() == ESP_ERR_INVALID_STATE,
          "connect() без конфигурации -> ESP_ERR_INVALID_STATE");
    CHECK(wifi_mock_netif_init_calls() == 1, "стек поднят до проверки конфигурации");
    CHECK(wifi_mock_init_calls() == 1, "esp_wifi_init вызван");
    CHECK(wifi_mock_set_config_calls() == 0, "до станции дело не дошло: set_config не звали");
    CHECK(wifi_mock_start_calls() == 0, "esp_wifi_start не звали");
    CHECK(wifi_mock_connect_calls() == 0, "подключения не было");
    CHECK(!kouki::wifi().is_connected(), "связи нет");
}

static void test_connect_timeout()
{
    std::printf("[TEST] 2. Таймаут подключения: STA_START без адреса...\n");

    seed_complete_config();
    wifi_mock_reset();
    // Событие STA_START приходит, адрес — нет: connect() обязан выйти по таймауту.
    wifi_mock_set_fire_sta_start(true);
    wifi_mock_set_auto_got_ip(false, 0, 0);

    CHECK(kouki::wifi().connect() == ESP_ERR_TIMEOUT, "без адреса connect() -> ESP_ERR_TIMEOUT");
    CHECK(wifi_mock_connect_calls() == 1, "STA_START отправляет один esp_wifi_connect()");
    CHECK(!kouki::wifi().is_connected(), "связи нет");
    CHECK(kouki::wifi().state_seq() == 0, "счётчик изменений не двигался");
}

static void test_connect_happy_path()
{
    std::printf("[TEST] 3. Успешное подключение: стек, параметры, GOT_IP...\n");

    seed_complete_config();
    wifi_mock_reset();
    wifi_mock_set_fire_sta_start(true);
    wifi_mock_set_auto_got_ip(true, kIp, kGw);

    CHECK(kouki::wifi().connect() == ESP_OK, "connect() с адресом -> ESP_OK");

    CHECK(wifi_mock_netif_init_calls() == 1, "esp_netif_init вызван");
    CHECK(wifi_mock_create_sta_calls() == 1, "станция создана один раз");
    CHECK(wifi_mock_init_calls() == 1, "esp_wifi_init вызван");
    CHECK(wifi_mock_handler_count() == 2, "два обработчика: WIFI_EVENT и IP_EVENT");
    CHECK(wifi_mock_mode() == WIFI_MODE_STA, "режим WIFI_MODE_STA");
    CHECK(wifi_mock_config_interface() == WIFI_IF_STA, "конфигурация WIFI_IF_STA");
    CHECK(std::strcmp(wifi_mock_ssid(), "kouki-home") == 0, "SSID взят из Config");
    CHECK(std::strcmp(wifi_mock_password(), "secret-pass") == 0, "пароль взят из Config");
    CHECK(wifi_mock_authmode() == WIFI_AUTH_WPA2_PSK, "с паролем — WPA2_PSK");
    CHECK(wifi_mock_start_calls() == 1, "esp_wifi_start вызван");
    CHECK(wifi_mock_inactive_time() == 3, "inactive_time = 3 с (T95)");
    CHECK(wifi_mock_connect_calls() == 1, "STA_START -> esp_wifi_connect");

    CHECK(kouki::wifi().is_connected(), "после GOT_IP связь есть");
    CHECK(kouki::wifi().state_seq() == 1, "получение адреса — одно изменение состояния");
    CHECK(kouki_wifi_is_connected(), "C-фасад видит связь");
    CHECK(kouki_wifi_state_seq() == 1, "C-фасад видит счётчик");
}

static void test_disconnect_backoff()
{
    std::printf("[TEST] 4. Обрыв: backoff 1→2→4→8→16→16 с и таймер...\n");

    // Состояние после успешного подключения из теста 3.
    CHECK(kouki::wifi().is_connected(), "до обрыва связь есть");
    const uint32_t seq_before = kouki::wifi().state_seq();

    const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 16000};
    const int stops_baseline = wifi_mock_timer_stop_calls();
    const int connects_baseline = wifi_mock_connect_calls();
    for (int i = 0; i < 6; i++) {
        const int starts_before = wifi_mock_timer_start_calls();
        wifi_mock_fire_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);

        CHECK(!kouki::wifi().is_connected(), "после обрыва связи нет");
        CHECK(kouki::wifi().state_seq() == seq_before + (uint32_t)(i + 1),
              "каждый обрыв двигает счётчик на 1");
        CHECK(wifi_mock_timer_start_calls() == starts_before + 1,
              "обрыв запускает таймер повтора");
        CHECK(wifi_mock_last_timer_us() == (uint64_t)expected[i] * 1000,
              "интервал таймера = пауза backoff в мкс");
    }

    CHECK(wifi_mock_timer_stop_calls() == stops_baseline + 6,
          "перед каждым запуском таймер останавливают");
    CHECK(wifi_mock_connect_calls() == connects_baseline,
          "при живом таймере esp_wifi_connect не зовут");
}

static void test_got_ip_resets_backoff()
{
    std::printf("[TEST] 5. GOT_IP: backoff сброшен в 1 с, таймер остановлен...\n");

    // После серии обрывов пауза упёрлась в 16 с; адрес должен её сбросить.
    const int stops_before = wifi_mock_timer_stop_calls();
    ip_event_got_ip_t ev = {};
    ev.ip_info.ip.addr = kIp;
    ev.ip_info.gw.addr = kGw;
    wifi_mock_fire_event(IP_EVENT, IP_EVENT_STA_GOT_IP, &ev);

    CHECK(kouki::wifi().is_connected(), "после GOT_IP связь восстановлена");
    CHECK(wifi_mock_timer_stop_calls() == stops_before + 1, "GOT_IP останавливает таймер");

    // Следующий обрыв должен снова начать с 1 с — пауза сброшена.
    wifi_mock_fire_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    CHECK(wifi_mock_last_timer_us() == 1000 * 1000, "после GOT_IP backoff снова 1 с");

    // И снова поднимаем связь для следующих проверок.
    wifi_mock_fire_event(IP_EVENT, IP_EVENT_STA_GOT_IP, &ev);
    CHECK(kouki::wifi().is_connected(), "связь поднята обратно");
}

static void test_retry_timer_callback()
{
    std::printf("[TEST] 6. Срабатывание таймера: повтор подключения...\n");

    CHECK(wifi_mock_timer_arg() != nullptr, "таймер создан с arg = экземпляр");
    CHECK(wifi_mock_timer_name() != nullptr &&
              std::strcmp(wifi_mock_timer_name(), "wifi-retry") == 0,
          "имя таймера «wifi-retry» как раньше");

    const int connects_before = wifi_mock_connect_calls();
    wifi_mock_fire_retry_timer();
    CHECK(wifi_mock_connect_calls() == connects_before + 1,
          "таймер повтора зовёт esp_wifi_connect()");
}

static void test_timer_create_failure()
{
    std::printf("[TEST] 7. Таймер не создался: повтор без паузы...\n");

    seed_complete_config();
    wifi_mock_reset();
    wifi_mock_set_fire_sta_start(false);
    wifi_mock_set_auto_got_ip(false, 0, 0);
    wifi_mock_fail_next_timer_create(ESP_ERR_NO_MEM);

    // Без STA_START и без таймера: обрыв сразу уходит в esp_wifi_connect().
    // connect() здесь дойдёт до wait и вернёт таймаут — это ожидаемо.
    CHECK(kouki::wifi().connect() == ESP_ERR_TIMEOUT, "connect() без событий -> ESP_ERR_TIMEOUT");
    CHECK(wifi_mock_timer_create_calls() == 1, "попытка создать таймер была");
    CHECK(wifi_mock_connect_calls() == 0, "STA_START не пришёл, connect не звали");

    const int connects_before = wifi_mock_connect_calls();
    wifi_mock_fire_event(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    CHECK(wifi_mock_connect_calls() == connects_before + 1,
          "без таймера обрыв сразу зовёт esp_wifi_connect()");
    CHECK(wifi_mock_timer_start_calls() == 0, "таймер не запускали");
}

static void test_get_info()
{
    std::printf("[TEST] 8. get_info: IP и RSSI, состояние без связи...\n");

    // Связи нет (последний обрыв в тесте 7): буферы обнуляются.
    char ip[32];
    int8_t rssi = 99;
    std::memset(ip, 'x', sizeof(ip));
    CHECK(kouki_wifi_get_info(ip, sizeof(ip), &rssi) == ESP_ERR_INVALID_STATE,
          "без связи get_info -> ESP_ERR_INVALID_STATE");
    CHECK(ip[0] == '\0', "IP-буфер очищен");
    CHECK(rssi == 0, "RSSI обнулён");

    // NULL-буферы при отсутствии связи не роняют модуль.
    CHECK(kouki_wifi_get_info(NULL, 0, NULL) == ESP_ERR_INVALID_STATE,
          "NULL-аргументы без связи -> ESP_ERR_INVALID_STATE");

    // Поднимаем связь и закладываем адрес/RSSI.
    ip_event_got_ip_t ev = {};
    ev.ip_info.ip.addr = kIp;
    ev.ip_info.gw.addr = kGw;
    wifi_mock_fire_event(IP_EVENT, IP_EVENT_STA_GOT_IP, &ev);
    CHECK(kouki::wifi().is_connected(), "связь поднята");

    wifi_mock_set_ip(kIp);
    wifi_mock_set_ap(true, -57);
    std::memset(ip, 0, sizeof(ip));
    rssi = 0;
    CHECK(kouki_wifi_get_info(ip, sizeof(ip), &rssi) == ESP_OK, "со связью get_info -> ESP_OK");
    CHECK(std::strcmp(ip, "192.168.1.42") == 0, "IP форматируется как раньше");
    CHECK(rssi == -57, "RSSI из esp_wifi_sta_get_ap_info");
    CHECK(wifi_mock_get_ip_info_calls() == 1, "esp_netif_get_ip_info вызван один раз");

    // NULL-аргументы при связи: адрес не запрашивают, RSSI не трогают.
    CHECK(kouki::wifi().get_info(NULL, 0, NULL) == ESP_OK, "NULL-аргументы со связью -> ESP_OK");
    CHECK(wifi_mock_get_ip_info_calls() == 1, "без буфера IP не запрашивали");

    // Нет данных о точке доступа — RSSI 0, а не мусор.
    wifi_mock_set_ap(false, 0);
    rssi = 55;
    CHECK(kouki::wifi().get_info(nullptr, 0, &rssi) == ESP_OK, "get_info без AP -> ESP_OK");
    CHECK(rssi == 0, "без данных AP RSSI обнуляется");
}

static void test_facade_and_instance()
{
    std::printf("[TEST] 9. Фасад и класс — один экземпляр...\n");

    CHECK(&kouki::wifi() == &kouki::wifi(), "wifi() — один экземпляр");
    CHECK(kouki::wifi().is_connected() == kouki_wifi_is_connected(),
          "фасад и метод видят одно состояние");
    CHECK(kouki::wifi().state_seq() == kouki_wifi_state_seq(),
          "фасад и метод видят один счётчик");
}

int main()
{
    std::printf("========================================\n");
    std::printf("      HOST CHECK: T121 Wifi             \n");
    std::printf("========================================\n");

    test_incomplete_config();
    test_connect_timeout();
    test_connect_happy_path();
    test_disconnect_backoff();
    test_got_ip_resets_backoff();
    test_retry_timer_callback();
    test_timer_create_failure();
    test_get_info();
    test_facade_and_instance();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d проверок успешно пройдено!\n", g_checks);
    std::printf("========================================\n");
    return 0;
}
