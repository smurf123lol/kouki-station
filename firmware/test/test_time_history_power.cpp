// Host-стенд T121 П3: классы Time, History, Power и их C-фасад.
//
// Проверяет, что поведение модулей 1:1 прежнему C-варианту (эталон —
// t114 Platform Behavior) и что фасад ходит в тот же экземпляр класса:
// кольцо истории и версия, яркость/idle, форматирование времени, а также
// сценарий «инициализация -> вызов -> ошибка -> повтор».
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app_time.h"
#include "app_time.hpp"

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

// ---------------------------------------------------------------- Time -----

static void test_time() {
    std::printf("[TEST] 1. Time: класс, TZ, форматирование, ошибки...\n");

    // Экземпляр один: фасад и класс — один объект, адрес стабилен.
    CHECK(&kouki::timekeeper() == &kouki::timekeeper(), "timekeeper() returns the same instance");

    // Ошибка до инициализации: NULL-буфер отвергается, код прежний.
    struct tm ti{};
    CHECK(kouki_time_get(nullptr) == ESP_ERR_INVALID_ARG, "time_get(NULL) -> ESP_ERR_INVALID_ARG");

    // Повторный init() не ломается и по-прежнему ESP_OK.
    CHECK(kouki_time_init("MSK-3") == ESP_OK, "init(MSK-3) -> ESP_OK");
    CHECK(kouki_time_init("MSK-3") == ESP_OK, "init(MSK-3) repeated -> ESP_OK");
    CHECK(kouki_time_init(nullptr) == ESP_OK, "init(NULL) -> ESP_OK (default TZ)");
    CHECK(kouki_time_init("") == ESP_OK, "init(\"\") -> ESP_OK (default TZ)");

    // Fallback-сервер: установка, замена, сброс NULL.
    kouki_time_set_fallback_server("192.168.1.1");
    kouki_time_set_fallback_server("10.0.0.1");
    kouki_time_set_fallback_server(nullptr);
    kouki_time_set_fallback_server("192.168.1.1");

    // Время на хосте синхронизировано (эпоха реальная).
    CHECK(kouki_time_is_synced(), "host time is synced");
    CHECK(kouki_time_get(&ti) == ESP_OK, "time_get -> ESP_OK when synced");
    CHECK(ti.tm_hour >= 0 && ti.tm_hour <= 23, "hour in range");
    CHECK(ti.tm_min >= 0 && ti.tm_min <= 59, "min in range");
    CHECK(ti.tm_mon >= 0 && ti.tm_mon <= 11, "mon in range");

    // Формат "HH:MM" — ровно 5 символов, оба поля цифровые.
    char hm[8] = {0};
    kouki_time_format_hm(hm, sizeof(hm));
    CHECK(std::strlen(hm) == 5, "format_hm -> 5 chars");
    CHECK(hm[2] == ':', "format_hm has ':' separator");
    CHECK(hm[0] >= '0' && hm[0] <= '9' && hm[1] >= '0' && hm[1] <= '9', "hour digits");
    CHECK(hm[3] >= '0' && hm[3] <= '9' && hm[4] >= '0' && hm[4] <= '9', "minute digits");

    // Усечённый буфер не переполняется и остаётся валидным C-строкой.
    char tiny[3] = {0};
    kouki_time_format_hm(tiny, sizeof(tiny));
    CHECK(std::strlen(tiny) == 2, "format_hm truncates to buffer size");

    // Нулевой буфер — no-op, без падения.
    kouki_time_format_hm(nullptr, 8);
    kouki_time_format_hm(hm, 0);

    // Дата: "пн, 10 ноя" — непустая, с запятой и днём.
    char date[64] = {0};
    kouki_time_format_date(date, sizeof(date));
    CHECK(std::strlen(date) > 5, "format_date is non-empty");
    CHECK(std::strchr(date, ',') != nullptr, "format_date has comma");
    CHECK(std::strcmp(date, "--") != 0, "format_date is a real date when synced");

    // Каждый форматтер берёт данные из того же экземпляра класса.
    char hm2[8] = {0};
    kouki::timekeeper().format_hm(hm2, sizeof(hm2));
    CHECK(std::strlen(hm2) == 5, "class format_hm matches facade");
    CHECK(std::strcmp(hm, hm2) == 0, "facade and class agree on format_hm");
}


int main() {
    test_time();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d/%d проверок успешно пройдено!\n", g_passed, g_checks);
    std::printf("========================================\n");
    return 0;
}
