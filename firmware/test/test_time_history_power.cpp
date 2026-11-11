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

#include "app_history.h"
#include "app_history.hpp"
#include "app_power.h"
#include "app_power.hpp"
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

/**
 * Подмена ::time() для кейса «часы ещё не синхронизированы».
 *
 * Time::is_synced() на хосте смотрит два признака: атомарный m_synced (его
 * ставит колбэк SNTP) и эпоху от ::time() против порога 1700000000. Хост-часы
 * всегда синхронизированы, поэтому единственный способ добраться до ветки
 * «не синхронизировано» — вернуть из ::time() значение ниже порога.
 * Пока g_fake_now >= 0, подмена действует на весь бинарник; каждый тест обязан
 * вернуть -1, иначе следующие увидят 1970 год.
 */
static long g_fake_now = -1;

extern "C" time_t time(time_t *out)
{
    if (g_fake_now >= 0) {
        if (out != nullptr) {
            *out = (time_t)g_fake_now;
        }
        return (time_t)g_fake_now;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (out != nullptr) {
        *out = ts.tv_sec;
    }
    return ts.tv_sec;
}

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

    // Пункт 3 ревью П3 (мутант t1): НЕсинхронизированное время.
    // Свежий экземпляр Time (m_synced = false) + подменённая эпоха ниже порога
    // дают ветку «не синхронизировано»: get() -> ESP_ERR_INVALID_STATE,
    // format_hm -> "--:--", format_date -> "--".
    g_fake_now = 1000;  // 1970 год, заведомо ниже порога 1700000000
    {
        kouki::Time unsynced;
        CHECK(!unsynced.is_synced(), "свежий Time с эпохой 1970 не синхронизирован");
        struct tm broken{};
        CHECK(unsynced.get(&broken) == ESP_ERR_INVALID_STATE,
              "get() при несинхронизированном времени -> ESP_ERR_INVALID_STATE");

        char unset_hm[8] = {0};
        unsynced.format_hm(unset_hm, sizeof(unset_hm));
        CHECK(std::strcmp(unset_hm, "--:--") == 0,
              "format_hm при несинхронизированном времени -> \"--:--\"");

        char unset_date[8] = {0};
        unsynced.format_date(unset_date, sizeof(unset_date));
        CHECK(std::strcmp(unset_date, "--") == 0,
              "format_date при несинхронизированном времени -> \"--\"");
    }
    g_fake_now = -1;  // важно: дальше идут тесты, которым нужна настоящая эпоха
    CHECK(kouki_time_is_synced(), "после возврата реальных часов время снова синхронно");
}

// ------------------------------------------------------------- History -----

static kouki_voice_result_t make_res(const char *transcript, const char *answer,
                                    const char *intent, const char *outcome,
                                    const char *title) {
    kouki_voice_result_t res{};
    std::snprintf(res.transcript, sizeof(res.transcript), "%s", transcript);
    std::snprintf(res.answer, sizeof(res.answer), "%s", answer);
    std::snprintf(res.intent, sizeof(res.intent), "%s", intent);
    std::snprintf(res.outcome, sizeof(res.outcome), "%s", outcome);
    std::snprintf(res.task_title, sizeof(res.task_title), "%s", title);
    return res;
}

static void test_history() {
    std::printf("[TEST] 2. History: кольцо, версия, фильтры, фасад...\n");

    CHECK(&kouki::history() == &kouki::history(), "history() returns the same instance");

    // Повторный init() — no-op, данные не теряются.
    kouki_history_init();
    kouki_history_init();
    kouki_history_clear();
    CHECK(kouki_history_count() == 0, "history empty after init+clear");

    kouki_history_item_t item{};
    CHECK(!kouki_history_get(0, &item), "get(0) on empty -> false");
    CHECK(!kouki_history_get(0, nullptr), "get(0, NULL) -> false");

    const uint32_t v0 = kouki_history_version();
    CHECK(kouki::history().version() == v0, "facade version == class version");

    // NULL-результат игнорируется, версия не растёт.
    kouki_history_push(nullptr);
    CHECK(kouki_history_version() == v0, "push(NULL) does not bump version");

    // Пустой результат (нет ни транскрипта, ни ответа) игнорируется.
    kouki_voice_result_t empty{};
    kouki_history_push(&empty);
    CHECK(kouki_history_version() == v0, "push(empty) does not bump version");
    CHECK(kouki_history_count() == 0, "history still empty");

    // Заполняем кольцо: 12 записей при ёмкости 10.
    for (int i = 1; i <= 12; i++) {
        char t[32], a[32], title[32];
        std::snprintf(t, sizeof(t), "Запрос %d", i);
        std::snprintf(a, sizeof(a), "Ответ %d", i);
        std::snprintf(title, sizeof(title), "Задача %d", i);
        kouki_voice_result_t res = make_res(t, a, "add_task", "created", title);
        kouki_history_push(&res);
    }

    CHECK(kouki_history_count() == 10, "capacity clamped at 10");
    CHECK(kouki_history_version() == v0 + 12, "version +1 per push");
    CHECK(kouki::history().count() == 10, "facade count == class count");

    // index 0 — самый свежий.
    CHECK(kouki_history_get(0, &item), "get(0) after fills");
    CHECK(std::strcmp(item.transcript, "Запрос 12") == 0, "index 0 is the newest");
    CHECK(std::strcmp(item.title, "Задача 12") == 0, "index 0 title");
    CHECK(std::strcmp(item.answer, "Ответ 12") == 0, "index 0 answer");
    CHECK(std::strcmp(item.outcome, "created") == 0, "index 0 outcome");
    CHECK(item.is_task, "index 0 is_task");
    CHECK(item.is_answer, "index 0 is_answer");
    CHECK(std::strlen(item.timestamp_hm) == 5, "index 0 timestamp is HH:MM");

    // index 9 — самый старый из оставшихся (1 и 2 вытеснены).
    CHECK(kouki_history_get(9, &item), "get(9)");
    CHECK(std::strcmp(item.transcript, "Запрос 3") == 0, "index 9 is the oldest remaining");
    CHECK(!kouki_history_get(10, &item), "get(10) out of range -> false");

    // Вторая перезапись кольца: head снова прошёл через ноль.
    kouki_history_clear();
    CHECK(kouki_history_version() == v0 + 13, "clear bumps version by 1");
    for (int i = 0; i < 25; i++) {
        char t[32];
        std::snprintf(t, sizeof(t), "Цикл %d", i);
        kouki_voice_result_t res = make_res(t, "", "ask", "", "");
        kouki_history_push(&res);
    }
    CHECK(kouki_history_count() == 10, "capacity still 10 after 25 pushes");
    CHECK(kouki_history_version() == v0 + 13 + 25, "version counts every push");
    CHECK(kouki_history_get(0, &item), "get(0) after many wraps");
    CHECK(std::strcmp(item.transcript, "Цикл 24") == 0, "newest is the last pushed");
    CHECK(!item.is_task, "ask without task markers is not a task");
    CHECK(item.is_answer, "intent=ask marks item as answer");
    CHECK(kouki_history_get(9, &item), "get(9) after many wraps");
    CHECK(std::strcmp(item.transcript, "Цикл 15") == 0, "oldest remaining after wraps");

    kouki_history_clear();
    CHECK(kouki_history_count() == 0, "cleared");
    CHECK(!kouki_history_get(0, &item), "get(0) after clear -> false");

    // Пункт 2 ревью П3 (мутант h5): повторный init() ПОСЛЕ заполнения не должен
    // обнулять кольцо. Раньше оба init() вызывались до push, поэтому снятый
    // guard `if (m_items != nullptr) return;` оставался незамеченным: тест
    // просто не видел состояния, которое guard защищает.
    for (int i = 1; i <= 3; i++) {
        // Кириллица в UTF-8 занимает два байта на символ: «До повторного init N»
        // это 33 байта, поэтому буфер заметно больше 32.
        char t[64];
        std::snprintf(t, sizeof(t), "До повторного init %d", i);
        kouki_voice_result_t res = make_res(t, "", "ask", "", "");
        kouki_history_push(&res);
    }
    CHECK(kouki_history_count() == 3, "три записи до повторного init");
    const uint32_t version_before = kouki_history_version();

    kouki_history_init();  // мутант h5 здесь обнулил бы head/count/version

    CHECK(kouki_history_count() == 3, "повторный init() сохранил записи");
    CHECK(kouki_history_version() == version_before, "повторный init() не сбросил версию");
    CHECK(kouki_history_get(0, &item), "get(0) после повторного init");
    CHECK(std::strcmp(item.transcript, "До повторного init 3") == 0,
          "самая свежая запись на месте после повторного init");
    CHECK(kouki_history_get(2, &item), "get(2) после повторного init");
    CHECK(std::strcmp(item.transcript, "До повторного init 1") == 0,
          "старейшая запись на месте после повторного init");

    // И кольцо продолжает работать после этого init().
    kouki_voice_result_t more = make_res("После init", "", "ask", "", "");
    kouki_history_push(&more);
    CHECK(kouki_history_count() == 4, "push после повторного init работает");
    CHECK(kouki_history_version() == version_before + 1, "версия продолжает расти");

    kouki_history_clear();
    CHECK(kouki_history_count() == 0, "очищено перед следующим блоком");
}

// --------------------------------------------------------------- Power -----

static void test_power() {
    std::printf("[TEST] 3. Power: димминг, насыщение, активность, фасад...\n");

    CHECK(&kouki::power() == &kouki::power(), "power() returns the same instance");

    // Повторный init() сбрасывает состояние.
    kouki_power_init();
    kouki_power_init();
    CHECK(kouki_power_get_brightness() == 100, "init -> brightness 100");
    CHECK(kouki_power_get_idle_ms() == 0, "init -> idle 0");

    // 30 с покоя — яркость ещё 100%.
    for (int i = 0; i < 30; i++) {
        kouki_power_tick(1000);
    }
    CHECK(kouki_power_get_idle_ms() == 30000, "idle 30000 after 30 s");
    CHECK(kouki_power_get_brightness() == 100, "brightness 100 at 30 s");
    CHECK(kouki::power().idle_ms() == 30000, "facade idle == class idle");
    CHECK(kouki::power().brightness() == 100, "facade brightness == class brightness");

    // Переход через порог 45 с: яркость плавно уходит к 20%.
    kouki_power_tick(1000);  // 31
    kouki_power_tick(1000);  // ... порог ещё не пройден
    for (int i = 0; i < 20; i++) {
        kouki_power_tick(1000);
    }
    CHECK(kouki_power_get_idle_ms() == 52000, "idle 52000");
    CHECK(kouki_power_get_brightness() == 20, "brightness dimmed to 20");

    // 0-мс тик не меняет idle и не ломает яркость.
    const uint32_t before = kouki_power_get_idle_ms();
    kouki_power_tick(0);
    CHECK(kouki_power_get_idle_ms() == before, "tick(0) keeps idle");

    // До 179 с держится 20%.
    while (kouki_power_get_idle_ms() < 179000) {
        kouki_power_tick(1000);
    }
    CHECK(kouki_power_get_brightness() == 20, "brightness 20 before 180 s");

    // Порог 180 с: затухание в 0%.
    for (int i = 0; i < 6; i++) {
        kouki_power_tick(1000);
    }
    CHECK(kouki_power_get_idle_ms() >= 185000, "idle >= 185000");
    CHECK(kouki_power_get_brightness() == 0, "brightness 0 after 180 s");

    // Насыщение idle — защита от переполнения uint32.
    for (int i = 0; i < 50; i++) {
        kouki_power_tick(1000);
    }
    CHECK(kouki_power_get_idle_ms() <= 186000, "idle saturates");
    CHECK(kouki_power_get_brightness() == 0, "brightness stays 0 while idle");

    // Активность: idle = 0, яркость мгновенно 100%.
    kouki_power_notify_activity();
    CHECK(kouki_power_get_idle_ms() == 0, "activity resets idle");
    CHECK(kouki_power_get_brightness() == 100, "activity restores 100");

    // Большой dt_ms не переполняет яркость и не уходит ниже нуля.
    kouki_power_init();
    for (int i = 0; i < 200; i++) {
        kouki_power_tick(1000);  // до 200 с, давно за порогом OFF
    }
    CHECK(kouki_power_get_brightness() == 0, "large total dt ends at 0");
    kouki_power_notify_activity();
    CHECK(kouki_power_get_brightness() == 100, "activity after long idle restores 100");
}

int main() {
    test_time();
    test_history();
    test_power();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d/%d проверок успешно пройдено!\n", g_passed, g_checks);
    std::printf("========================================\n");
    return 0;
}
