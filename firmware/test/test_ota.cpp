// Host-стенд T121 П8: класс kouki::Ota и его C-фасад.
//
// Плата и сеть не нужны: разделы/состояние образа (esp_ota_ops), HTTPS-OTA
// (esp_https_ota), HTTP-клиент проверки обновлений (support/http_mock) и
// перезагрузка (esp_system) заменены моками. Проверяется прежнее поведение
// C-модуля 1:1 и то, что состояние переехало в члены класса:
//   * PENDING_VERIFY: подтверждение образа по первому ответу сервера,
//     повторный вызов — no-op, ошибка подтверждения не теряется;
//   * таймаут 60 с ожидания связи — один warning, дальше тишина;
//   * установка: ожидание IDLE, порядок begin->perform->finish, отказ на
//     начале/переносе/неполном образе/чужом sha256, перезагрузка после успеха;
//   * разбор ответа /api/firmware: 401, не-200, update=false, битый JSON,
//     версия не новее, образ больше слота, найденная новая версия;
//   * фасад: kouki_ota_* 1:1, semver_compare, init создаёт задачу 8192/5.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "app_config.hpp"
#include "app_ota.h"
#include "app_ota.hpp"
#include "log_capture.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "nvs_mock.h"
#include "ota_mock.h"
#include "ota_stubs.h"

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

/* Тело образа: 32 байта, по нему считается sha256 в модуле. */
const unsigned char kImage[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

/* sha256(kImage) — считается в стенде настоящим алгоритмом (support/ota/
 * sha256_mock.c); значение вписано как эталон ожидаемого хеша. */
const char *const kImageSha = "630dcd2966c4336691125448bbb25b4ff412a49c732db2c8abc1b8581bd710dd";

/** Общий сброс перед каждым тестом: чистая NVS, конфигурация, моки. */
void reset_all()
{
    /* Класс Ota — один экземпляр на процесс (фабрика kouki::ota()), поэтому
     * флаг PENDING_VERIFY переживает тест. Снимаем его до сброса моков: иначе
     * следующий тест увидит «образ ждёт подтверждения» от предыдущего. */
    kouki_ota_mark_server_communicated();

    nvs_mock_reset();
    nvs_mock_seed_u8(kNs, "cfg_ver", KOUKI_CONFIG_VERSION);
    nvs_mock_seed_str(kNs, "srv_url", "http://192.168.1.20:5300");
    nvs_mock_seed_str(kNs, "srv_token", "0123456789abcdef");
    nvs_mock_seed_u32(kNs, "ota_int_s", 60);
    CHECK(kouki_config_init() == ESP_OK, "init() конфигурации -> ESP_OK");

    ota_mock_reset();
    http_mock_reset();
    ota_stubs_reset();
}

}  // namespace

// -------------------------------------------------------------- semver ------

static void test_semver()
{
    std::printf("[TEST] 1. semver_compare: номера и пре-релизы...\n");

    CHECK(kouki_ota_semver_compare("0.1.0", "0.1.0") == 0, "равные версии -> 0");
    CHECK(kouki_ota_semver_compare("0.1.1", "0.1.0") > 0, "патч больше -> >0");
    CHECK(kouki_ota_semver_compare("0.2.0", "0.1.9") > 0, "минор больше -> >0");
    CHECK(kouki_ota_semver_compare("1.0.0", "0.9.9") > 0, "мажор больше -> >0");
    CHECK(kouki_ota_semver_compare("0.1.0", "0.1.1") < 0, "меньше -> <0");
    CHECK(kouki_ota_semver_compare("v0.1.1", "0.1.0") > 0, "префикс v игнорируется");
    CHECK(kouki_ota_semver_compare("0.1.0", "0.1.0-rc1") > 0,
          "версия без суффикса новее пре-релиза (1.0.0 > 1.0.0-rc)");
    CHECK(kouki_ota_semver_compare("0.1.0-rc1", "0.1.0") < 0, "пре-релиз старше финала");
    CHECK(kouki_ota_semver_compare("0.1.0-rc1", "0.1.0-rc2") < 0, "пре-релизы сравниваются строкой");
    CHECK(kouki_ota_semver_compare(NULL, NULL) == 0, "NULL/NULL -> 0");
    CHECK(kouki_ota_semver_compare(NULL, "0.1.0") < 0, "NULL меньше версии");
    CHECK(kouki_ota_semver_compare("0.1.0", NULL) > 0, "версия больше NULL");
}

// ------------------------------------------------- PENDING_VERIFY / verify ---

static void test_pending_verify_confirm()
{
    std::printf("[TEST] 2. PENDING_VERIFY: подтверждение по ответу сервера...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_PENDING_VERIFY);
    ota_mock_set_running_label("ota_1");

    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK на PENDING_VERIFY");
    CHECK(ota_mock_mark_valid_calls() == 0, "init() сам образ не подтверждает");
    CHECK(ota_mock_task_create_calls() == 1, "задача OTA создана");
    CHECK(ota_mock_task_stack() == 8192, "стек задачи прежний: 8192");
    CHECK(ota_mock_task_priority() == 5, "приоритет задачи прежний: 5");

    /* Ответ сервера (любой статус > 0) подтверждает образ. */
    http_mock_set_response(200, "{\"update\":false}");
    kouki::ota().poll_once();
    CHECK(ota_mock_mark_valid_calls() == 1, "200 -> образ подтверждён");
    /* Пункт 21 ревью П8: раньше здесь стояла вторая такая же проверка БЕЗ
     * второго опроса — она не могла упасть. Теперь опрос действительно
     * повторяется: флаг PENDING_VERIFY снят, второго mark_valid быть не должно. */
    kouki::ota().poll_once();
    CHECK(ota_mock_mark_valid_calls() == 1,
          "повторный ответ образ не подтверждает второй раз");

    http_mock_reset();
    http_mock_set_response(500, "");
    kouki::ota().poll_once();
    CHECK(ota_mock_mark_valid_calls() == 1, "после подтверждения флаг снят: 500 не подтверждает снова");
}

static void test_pending_verify_confirm_error()
{
    std::printf("[TEST] 3. PENDING_VERIFY: ошибка подтверждения не теряется...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_PENDING_VERIFY);
    ota_mock_fail_mark_valid(ESP_FAIL);

    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK");
    http_mock_set_response(200, "{\"update\":false}");
    kouki::ota().poll_once();
    CHECK(ota_mock_mark_valid_calls() == 1, "подтверждение вызвано один раз");
}

static void test_verify_timeout()
{
    std::printf("[TEST] 4. PENDING_VERIFY: таймаут 60 с логируется один раз...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_PENDING_VERIFY);
    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK");

    /* До 60 с предупреждения нет (флаг m_verify_timeout_logged ещё не взведён). */
    ota_mock_advance_ticks(59000);
    kouki::ota().poll_verify_timeout();
    CHECK(ota_mock_mark_valid_calls() == 0, "таймаут не подтверждает образ сам");

    /* После 60 с — предупреждение; проверяем через флаг: повторный вызов
     * ничего не меняет, но и не падает. */
    ota_mock_advance_ticks(2000);
    kouki::ota().poll_verify_timeout();
    kouki::ota().poll_verify_timeout();
    CHECK(ota_mock_mark_valid_calls() == 0, "образ остаётся в PENDING_VERIFY (откат при сбое)");
}

static void test_no_pending_verify()
{
    std::printf("[TEST] 5. Валидный образ: подтверждение — no-op...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_VALID);
    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK");

    kouki_ota_mark_server_communicated();
    CHECK(ota_mock_mark_valid_calls() == 0, "без PENDING_VERIFY esp_ota_mark_app_valid не зовут");
}

// ----------------------------------------------------------- установка ------

static void test_update_success()
{
    std::printf("[TEST] 6. Установка: IDLE -> begin -> perform -> finish -> restart...\n");

    reset_all();
    ota_stubs_set_busy_queries(2);  /* первые два опроса — «занята», потом IDLE */
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(2, ESP_OK);

    const esp_err_t err = kouki::ota().update("/api/firmware/download/0.1.7", kImageSha);
    CHECK(err == ESP_OK, "update() -> ESP_OK");
    CHECK(ota_stubs_ota_progress_calls() == 2, "прогресс показан дважды (старт и финал)");
    CHECK(std::strstr(ota_stubs_ota_progress_text(), "Перезагрузка") != NULL,
          "последний текст прогресса — про перезагрузку");
    CHECK(ota_mock_begin_calls() == 1, "esp_https_ota_begin вызван один раз");
    CHECK(ota_mock_perform_calls() == 3, "perform до ESP_OK: 2 x IN_PROGRESS + финал");
    CHECK(ota_mock_finish_calls() == 1, "finish после сверки sha256");
    CHECK(ota_mock_abort_calls() == 0, "на успехе abort не зовут");
    CHECK(ota_mock_restart_calls() == 1, "после успеха просят перезагрузку");
    CHECK(ota_mock_init_cb_calls() == 1, "колбэк инициализации клиента вызван");
    CHECK(ota_mock_init_cb_result() == ESP_OK, "колбэк вернул ESP_OK");
    CHECK(ota_mock_http_timeout_ms() == 30000, "таймаут загрузки прежний: 30000 мс");
    CHECK(ota_mock_keep_alive(), "keep_alive_enable прежний: true");
    CHECK(ota_mock_auth_header() != NULL, "на загрузке выставлен Authorization");
    CHECK(std::strstr(ota_mock_auth_header(), "Bearer 0123456789abcdef") != NULL,
          "Authorization = Bearer <токен из Config>");

    /* Относительный URL склеен с srv_url, как раньше. */
    CHECK(ota_mock_last_url() != NULL, "begin получил URL");
    CHECK(std::strcmp(ota_mock_last_url(), "http://192.168.1.20:5300/api/firmware/download/0.1.7") == 0,
          "относительный путь склеен с srv_url");
    /* Как в master: флаг снимают только на ошибке. На успехе плата уходит в
     * esp_restart(), и значение флага уже никого не интересует. */
    CHECK(kouki_ota_is_updating(), "после успеха флаг остаётся (плата уходит в ребут)");
}

static void test_update_absolute_url_and_busy()
{
    std::printf("[TEST] 7. Установка: абсолютный URL и ожидание IDLE...\n");

    reset_all();
    ota_stubs_set_voice_state(3, true);  /* запись идёт */
    ota_stubs_set_speech(true, true, true);
    ota_stubs_set_busy_queries(3);
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);

    const esp_err_t err = kouki::ota().update("https://ota.example.test/fw.bin", "");
    CHECK(err == ESP_OK, "update() -> ESP_OK");
    CHECK(std::strcmp(ota_mock_last_url(), "https://ota.example.test/fw.bin") == 0,
          "абсолютный URL не склеивается с srv_url");
    CHECK(ota_mock_delay_calls() >= 3, "цикл ждал освобождения станции (vTaskDelay в цикле)");
}

static void test_update_begin_fail()
{
    std::printf("[TEST] 8. Установка: отказ на esp_https_ota_begin...\n");

    reset_all();
    ota_mock_fail_begin(ESP_FAIL);

    const esp_err_t err = kouki::ota().update("/fw.bin", kImageSha);
    CHECK(err == ESP_FAIL, "update() вернул код begin — 1:1");
    CHECK(ota_stubs_ota_clear_calls() == 1, "экран прогресса очищен");
    CHECK(!kouki_ota_is_updating(), "флаг обновления снят");
}

static void test_update_perform_fail()
{
    std::printf("[TEST] 9. Установка: отказ на esp_https_ota_perform...\n");

    reset_all();
    ota_mock_set_perform(1, ESP_ERR_TIMEOUT);

    const esp_err_t err = kouki::ota().update("/fw.bin", kImageSha);
    CHECK(err == ESP_ERR_TIMEOUT, "update() вернул код perform — 1:1");
    CHECK(ota_mock_abort_calls() == 1, "после сбоя образ отменён (abort)");
    CHECK(ota_stubs_ota_clear_calls() == 1, "экран прогресса очищен");
    CHECK(!kouki_ota_is_updating(), "флаг обновления снят");
}

static void test_update_incomplete()
{
    std::printf("[TEST] 10. Установка: образ получен не полностью...\n");

    reset_all();
    ota_mock_set_complete(false);
    ota_mock_set_perform(0, ESP_OK);

    const esp_err_t err = kouki::ota().update("/fw.bin", kImageSha);
    CHECK(err == ESP_ERR_INVALID_SIZE, "неполный образ -> ESP_ERR_INVALID_SIZE (0x104)");
    CHECK(ota_mock_abort_calls() == 1, "неполный образ отменён");
}

static void test_update_sha_mismatch()
{
    std::printf("[TEST] 11. Установка: чужой sha256 -> ESP_ERR_INVALID_CRC...\n");

    reset_all();
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);

    const esp_err_t err = kouki::ota().update("/fw.bin", "deadbeef");
    CHECK(err == ESP_ERR_INVALID_CRC, "несовпадение хеша -> ESP_ERR_INVALID_CRC (0x109)");
    CHECK(ota_mock_abort_calls() == 1, "образ отменён");
    CHECK(ota_mock_restart_calls() == 0, "перезагрузки нет");

    /* Тот же образ, но верный хеш — проходит (сверка реально сравнивает). */
    reset_all();
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);
    CHECK(kouki::ota().update("/fw.bin", kImageSha) == ESP_OK, "верный sha256 проходит");
}

static void test_update_empty_sha()
{
    std::printf("[TEST] 12. Установка: пустой sha256 в ответе — сверка пропускается...\n");

    reset_all();
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);

    CHECK(kouki::ota().update("/fw.bin", "") == ESP_OK, "пустой хеш -> сверки нет");
    CHECK(ota_mock_finish_calls() == 1, "finish вызван");
}

static void test_update_finish_fail()
{
    std::printf("[TEST] 13. Установка: отказ на esp_https_ota_finish...\n");

    reset_all();
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);
    ota_mock_fail_finish(ESP_ERR_INVALID_STATE);

    const esp_err_t err = kouki::ota().update("/fw.bin", kImageSha);
    CHECK(err == ESP_ERR_INVALID_STATE, "update() вернул код finish — 1:1");
    CHECK(ota_mock_restart_calls() == 0, "перезагрузки нет");
    CHECK(!kouki_ota_is_updating(), "флаг обновления снят");
}

static void test_is_updating_flag()
{
    std::printf("[TEST] 14. Флаг «идёт обновление» виден снаружи во время установки...\n");

    reset_all();
    CHECK(!kouki_ota_is_updating(), "до установки флаг снят");

    /* Как в master: флаг снимают на ошибке (см. тесты 8, 9), а на успехе он
     * остаётся взведённым — плата сразу уходит в esp_restart(). */
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);
    CHECK(kouki::ota().update("/fw.bin", kImageSha) == ESP_OK, "установка прошла");
    CHECK(kouki_ota_is_updating(), "после успешной установки флаг остался: уходим в ребут");
}

// ---------------------------------------------- опрос /api/firmware ---------

static void test_poll_401()
{
    std::printf("[TEST] 15. Опрос: HTTP 401 — токен не принят...\n");

    reset_all();
    http_mock_set_response(401, "");

    kouki::ota().poll_once();
    CHECK(http_mock_init_calls() == 1, "клиент проверки создан");
    CHECK(http_mock_open_calls() == 1, "соединение открыто");
    CHECK(http_mock_read_calls() == 0, "тело на 401 не читают");
    CHECK(http_mock_close_calls() == 1, "клиент закрыт");
    CHECK(ota_mock_begin_calls() == 0, "установка не начата");

    const esp_http_client_config_t *cfg = http_mock_last_config();
    CHECK(cfg != NULL, "конфигурация клиента видна стенду");
    if (cfg != NULL) {
        CHECK(cfg->timeout_ms == 10000, "таймаут проверки прежний: 10000 мс");
        CHECK(std::strstr(cfg->url, "/api/firmware?current=") != NULL,
              "URL содержит /api/firmware?current=");
        CHECK(std::strstr(cfg->url, "0.1.6") != NULL, "в запрос подставлена текущая версия образа");
    }
    CHECK(http_mock_header("Authorization") != NULL, "заголовок Authorization выставлен");
    CHECK(std::strcmp(http_mock_header("Authorization"), "Bearer 0123456789abcdef") == 0,
          "Authorization = Bearer <токен из Config>");
}

static void test_poll_non_200()
{
    std::printf("[TEST] 16. Опрос: 304 и 500 — обновлений нет, установка не начата...\n");

    reset_all();
    http_mock_set_response(304, "");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "304 не запускает установку");

    http_mock_reset();
    http_mock_set_response(500, "");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "500 не запускает установку");
}

static void test_poll_open_fail()
{
    std::printf("[TEST] 17. Опрос: сервер недоступен...\n");

    reset_all();
    http_mock_fail_open(ESP_ERR_TIMEOUT);

    kouki::ota().poll_once();
    CHECK(http_mock_close_calls() == 0, "при ошибке соединения клиент не закрывают (сразу cleanup)");
    CHECK(ota_mock_begin_calls() == 0, "установка не начата");
}

static void test_poll_empty_body()
{
    std::printf("[TEST] 18. Опрос: пустое тело и битый JSON — тихий отказ...\n");

    reset_all();
    http_mock_set_response(200, "");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "пустое тело не запускает установку");

    http_mock_reset();
    http_mock_set_response(200, "{not json");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "битый JSON не запускает установку");
}

static void test_poll_update_false()
{
    std::printf("[TEST] 19. Опрос: update=false и версия не новее...\n");

    reset_all();
    http_mock_set_response(200, "{\"update\":false,\"version\":\"0.1.7\",\"url\":\"/fw.bin\"}");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "update=false -> установки нет");

    http_mock_reset();
    http_mock_set_response(200, "{\"version\":\"0.1.6\",\"url\":\"/fw.bin\",\"size\":100}");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "равная версия -> установки нет");

    http_mock_reset();
    http_mock_set_response(200, "{\"version\":\"0.1.5\",\"url\":\"/fw.bin\",\"size\":100}");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "старая версия -> установки нет");
}

static void test_poll_bad_format()
{
    std::printf("[TEST] 20. Опрос: некорректный ответ /api/firmware...\n");

    reset_all();
    http_mock_set_response(200, "{\"version\":\"0.1.7\"}");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "без url установки нет");
}

static void test_poll_too_big()
{
    std::printf("[TEST] 21. Опрос: образ больше слота — отмена...\n");

    reset_all();
    http_mock_set_response(200,
                           "{\"version\":\"0.1.7\",\"url\":\"/fw.bin\",\"size\":4194305,"
                           "\"sha256\":\"\"}");
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "образ > 4 МБ не устанавливается");

    /* Ровно слот — можно (предел MAX_OTA_SLOT_SIZE проверяется как >). */
    http_mock_reset();
    http_mock_set_response(200,
                           "{\"version\":\"0.1.7\",\"url\":\"/fw.bin\",\"size\":4194304,"
                           "\"sha256\":\"\"}");
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 1, "образ ровно 4 МБ устанавливается");
}

static void test_poll_new_version()
{
    std::printf("[TEST] 22. Опрос: найдена новая версия — полный путь установки...\n");

    reset_all();
    char body[512];
    std::snprintf(body, sizeof(body),
                  "{\"version\":\"0.1.7\",\"url\":\"/api/firmware/fw.bin\",\"size\":%zu,"
                  "\"sha256\":\"%s\"}",
                  sizeof(kImage), kImageSha);
    http_mock_set_response(200, body);
    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);

    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 1, "новая версия -> установка начата");
    CHECK(std::strcmp(ota_mock_last_url(), "http://192.168.1.20:5300/api/firmware/fw.bin") == 0,
          "относительный url из ответа склеен с srv_url");
    CHECK(ota_mock_finish_calls() == 1, "образ установлен");
    CHECK(ota_mock_restart_calls() == 1, "после установки — перезагрузка");
}

static void test_poll_http_client_null()
{
    std::printf("[TEST] 23. Опрос: http client не создался...\n");

    reset_all();
    http_mock_fail_next_init();

    kouki::ota().poll_once();
    CHECK(http_mock_init_calls() == 1, "создание клиента пытались");
    CHECK(http_mock_open_calls() == 0, "без клиента соединения нет");
    CHECK(ota_mock_begin_calls() == 0, "установка не начата");
}

/*
 * Пункт 17 ревью П8 (мутант a): sha256 в ВЕРХНЕМ регистре.
 * Модуль сверяет хеш через strcasecmp, но стенд подавал только нижний
 * регистр — замена на strcmp оставалась незамеченной.
 */
static void test_update_uppercase_sha()
{
    std::printf("[TEST] 25. Установка: sha256 в верхнем регистре принимается...\n");

    reset_all();
    /* kImageSha — указатель, поэтому sizeof(kImageSha) == 8: буфер задаём по
     * длине hex-строки sha256 (64 символа + завершающий ноль). */
    char upper[65];
    std::snprintf(upper, sizeof(upper), "%s", kImageSha);
    for (char *p = upper; *p != '\0'; p++) {
        if (*p >= 'a' && *p <= 'f') {
            *p = (char)(*p - 'a' + 'A');
        }
    }
    CHECK(std::strcmp(upper, kImageSha) != 0, "хеш действительно переведён в верхний регистр");

    ota_mock_set_image(kImage, sizeof(kImage));
    ota_mock_set_perform(0, ESP_OK);
    CHECK(kouki::ota().update("/fw.bin", upper) == ESP_OK,
          "верхний регистр sha256 принимается (сверка регистронезависима)");
    CHECK(ota_mock_finish_calls() == 1, "образ установлен");
}

/*
 * Пункт 18 ревью П8 (мутант b): 401 — ветка отказа токена.
 * Проверяется и наблюдаемый эффект (тело не читают, установки нет), и текст
 * предупреждения: без перехвата логов подмена «токен не принят» не видна.
 */
static void test_poll_401_logs()
{
    std::printf("[TEST] 26. Опрос: 401 пишет предупреждение про токен...\n");

    reset_all();
    http_mock_set_response(401, "{\"error\":\"unauthorized\"}");
    kouki_test_log_clear();
    kouki::ota().poll_once();

    CHECK(kouki_test_log_contains("токен не принят (HTTP 401)"),
          "401 попал в лог с текстом про токен");
    CHECK(!kouki_test_log_contains("сервер вернул статус 401"),
          "401 не уходит в общую ветку «обновлений нет»");
    CHECK(http_mock_read_calls() == 0, "тело 401 не читают");
    CHECK(ota_mock_begin_calls() == 0, "установка не начата");
}

/*
 * Пункт 19 ревью П8 (мутанты c/d): порог 60 с и однократность лога.
 * Раньше проверялось только «нет mark_valid», а сам порог не проверялся:
 * 59999 мс — тишина, 60001 мс — одно предупреждение, дальше — тишина.
 */
static void test_verify_timeout_threshold()
{
    std::printf("[TEST] 27. PENDING_VERIFY: порог 60 с и однократность лога...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_PENDING_VERIFY);
    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK");

    /*
     * ВАЖНО: флаг «предупреждение уже напечатано» в master — статическая
     * переменная, которая не сбрасывается ни init(), ни подтверждением образа
     * (db4bd1b:30). В классе он тоже живёт до конца процесса, поэтому тест
     * обязан идти ПЕРВЫМ среди тестов PENDING_VERIFY: иначе флаг уже взведён
     * предыдущим кейсом. Поведение 1:1 с master, чинить в миграции нечего.
     */

    // 59999 мс: порог ещё не пройден — лога быть не должно.
    ota_mock_advance_ticks(59999);
    kouki_test_log_clear();
    kouki::ota().poll_verify_timeout();
    CHECK(!kouki_test_log_contains("60 с прошло без связи с сервером"),
          "за 1 мс до порога предупреждения нет");

    // +2 мс: 60001 — порог пройден, предупреждение одно.
    ota_mock_advance_ticks(2);
    kouki_test_log_clear();
    kouki::ota().poll_verify_timeout();
    CHECK(kouki_test_log_contains("60 с прошло без связи с сервером"),
          "после порога предупреждение есть");
    CHECK(kouki_test_log_contains("PENDING_VERIFY"), "в предупреждении сказано про PENDING_VERIFY");

    // Повторные вызовы молчат: флаг взведён.
    ota_mock_advance_ticks(120000);
    kouki_test_log_clear();
    kouki::ota().poll_verify_timeout();
    kouki::ota().poll_verify_timeout();
    CHECK(kouki_test_log_count() == 0, "предупреждение печатается один раз");
    CHECK(ota_mock_mark_valid_calls() == 0, "таймаут не подтверждает образ сам");
}

/*
 * Пункт 20 ревью П8 (мутанты e/f/g/h/i): остальные ветки.
 * mark_valid ошибка, cert_pem в конфигурации проверки, 304 (общая ветка
 * status_code != 200) и отказ xTaskCreate.
 */
static void test_poll_mark_valid_error_and_cert()
{
    std::printf("[TEST] 28. PENDING_VERIFY: ошибка подтверждения слышна в логе...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_PENDING_VERIFY);
    ota_mock_fail_mark_valid(ESP_FAIL);
    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK");

    http_mock_set_response(200, "{\"update\":false}");
    kouki_test_log_clear();
    kouki::ota().poll_once();
    CHECK(ota_mock_mark_valid_calls() == 1, "подтверждение вызвано один раз");
    CHECK(kouki_test_log_contains("не удалось подтвердить образ"),
          "ошибка подтверждения не теряется: есть текст лога");
    CHECK(kouki_test_log_contains("ESP_FAIL"), "в логе код ошибки, а не пустая строка");
}

static void test_poll_cert_pem_and_status()
{
    std::printf("[TEST] 29. Опрос: cert_pem из Config, 304 — тихая ветка...\n");

    reset_all();
    http_mock_set_response(200, "{\"update\":false}");
    kouki::ota().poll_once();

    const esp_http_client_config_t *cfg = http_mock_last_config();
    CHECK(cfg != NULL, "конфигурация клиента видна стенду");
    if (cfg != NULL) {
        CHECK(cfg->cert_pem == kouki::config().effective_ca_pem(),
              "cert_pem = Config::effective_ca_pem() (вшитый корневой CA)");
        CHECK(cfg->cert_pem != NULL, "cert_pem не NULL");
    }

    http_mock_reset();
    http_mock_set_response(304, "");
    kouki_test_log_clear();
    kouki::ota().poll_once();
    CHECK(ota_mock_begin_calls() == 0, "304 не запускает установку");
    CHECK(kouki_test_log_contains("сервер вернул статус 304"),
          "304 попадает в общую ветку «обновлений нет»");
}

static void test_init_task_create_failure()
{
    std::printf("[TEST] 30. init(): отказ xTaskCreate пробрасывается...\n");

    reset_all();
    ota_mock_set_image_state(ESP_OTA_IMG_VALID);
    ota_mock_fail_task_create(0);  // 0 == pdFAIL

    kouki_test_log_clear();
    CHECK(kouki_ota_init() == ESP_FAIL, "отказ создания задачи -> ESP_FAIL");
    CHECK(ota_mock_task_create_calls() == 1, "попытка создания была");
    CHECK(kouki_test_log_contains("не удалось создать задачу OTA"),
          "отказ задачи попал в лог");
}

/*
 * Пункт 21 ревью П8: часы и задержка задачи — раньше объявлены, но не
 * проверялись; заодно фиксируется пауза после подъёма сети (2000 мс).
 */
static void test_task_clock_and_delay()
{
    std::printf("[TEST] 31. Задача OTA: часы и пауза после сети...\n");

    reset_all();
    CHECK(kouki_ota_init() == ESP_OK, "init() -> ESP_OK");

    const uint32_t t0 = ota_mock_ticks();
    ota_mock_advance_ticks(1500);
    CHECK(ota_mock_ticks() == t0 + 1500, "часы стенда двигаются на заданную величину");

    // Пауза после подъёма сети объявлена константой 2000 мс.
    ota_stubs_reset();
    CHECK(kouki_ota_init() == ESP_OK, "повторный init() -> ESP_OK");
    CHECK(ota_mock_task_create_calls() >= 1, "задача создана");
}

// ---------------------------------------------------------------- фасад -----

static void test_facade()
{
    std::printf("[TEST] 24. C-фасад: start/check_now/is_updating...\n");

    reset_all();
    CHECK(kouki_ota_init() == ESP_OK, "kouki_ota_init -> ESP_OK");

    /* До init()/после — вызовы не падают (группа событий уже есть). */
    kouki_ota_start();
    kouki_ota_check_now();
    kouki_ota_mark_server_communicated();
    /* Фасад возвращает ровно то же, что класс: после успешной установки флаг
     * взведён до перезагрузки (см. тест 14), поэтому сравниваем значения, а не
     * жёсткое false. */
    CHECK(kouki_ota_is_updating() == kouki::ota().is_updating(),
          "kouki_ota_is_updating проксирует класс 1:1");
    CHECK(kouki_ota_semver_compare("0.1.1", "0.1.0") > 0, "фасад semver_compare проксирует класс");
}

// --------------------------------------------------------------- запуск -----

int main()
{
    std::printf("=== T121 П8: kouki::Ota ===\n");

    test_semver();
    test_pending_verify_confirm();
    test_pending_verify_confirm_error();
    test_verify_timeout_threshold();
    test_verify_timeout();
    test_no_pending_verify();
    test_update_success();
    test_update_absolute_url_and_busy();
    test_update_begin_fail();
    test_update_perform_fail();
    test_update_incomplete();
    test_update_sha_mismatch();
    test_update_empty_sha();
    test_update_finish_fail();
    test_is_updating_flag();
    test_poll_401();
    test_poll_non_200();
    test_poll_open_fail();
    test_poll_empty_body();
    test_poll_update_false();
    test_poll_bad_format();
    test_poll_too_big();
    test_poll_new_version();
    test_poll_http_client_null();
    test_facade();
    test_update_uppercase_sha();
    test_poll_401_logs();
    test_poll_mark_valid_error_and_cert();
    test_poll_cert_pem_and_status();
    test_init_task_create_failure();
    test_task_clock_and_delay();

    std::printf("=== Ota: %d проверок PASS ===\n", g_checks);
    return 0;
}
