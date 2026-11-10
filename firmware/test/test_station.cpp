// Host-стенд T121 П13: объектный граф станции и порядок инициализации.
//
// Проверяется не текст комментария, а ФАКТИЧЕСКИЙ код: файлы читаются,
// комментарии вырезаются, и последовательность вызовов в теле
// kouki::station_init() / app_main() сверяется с прежним app_main master
// (`git show db4bd1b:firmware/main/main.c`, строки 152-173). Перестановка
// двух вызовов в kouki_api.cpp валит этот стенд.
//
// Почему по исходникам, а не линковкой: station_init() трогает весь образ
// (esp_board_init, LVGL, esp-sr), на хосте это не собрать. Зато порядок —
// это порядок ВЫЗОВОВ в одном теле, и его видно статически. Остальные
// инварианты (main.c — C-файл, единая точка сборки фабрик) тоже проверяются
// по исходникам и по тем же правилам, что AGENTS.md п.1/п.5.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kouki_api.h"

#ifndef KOUKI_MAIN_DIR
#define KOUKI_MAIN_DIR ""
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

std::string read_file(const std::string &path)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "FAIL: не открыть %s\n", path.c_str());
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

// Вырезает строчные и блочные комментарии, сохраняя переводы строк.

std::string strip_comments(const std::string &src)
{
    std::string out;
    out.reserve(src.size());
    bool line = false;
    bool block = false;
    bool in_str = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char next = (i + 1 < src.size()) ? src[i + 1] : '\0';
        if (line) {
            if (c == '\n') {
                line = false;
                out.push_back(c);
            }
            continue;
        }
        if (block) {
            if (c == '*' && next == '/') {
                block = false;
                ++i;
            } else if (c == '\n') {
                out.push_back(c);
            }
            continue;
        }
        if (in_str) {
            out.push_back(c);
            if (c == '\\' && i + 1 < src.size()) {
                out.push_back(src[++i]);
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '/' && next == '/') {
            line = true;
            ++i;
            continue;
        }
        if (c == '/' && next == '*') {
            block = true;
            ++i;
            continue;
        }
        if (c == '"') {
            in_str = true;
        }
        out.push_back(c);
    }
    return out;
}

/** Тело функции: от её '{' до парной '}' (скобки считаются по символам). */
std::string body_of(const std::string &src, const std::string &signature)
{
    const size_t pos = src.find(signature);
    if (pos == std::string::npos) {
        std::fprintf(stderr, "FAIL: не найдена функция %s\n", signature.c_str());
        std::exit(1);
    }
    const size_t open = src.find('{', pos + signature.size());
    if (open == std::string::npos) {
        std::fprintf(stderr, "FAIL: нет тела у %s\n", signature.c_str());
        std::exit(1);
    }
    int depth = 0;
    for (size_t i = open; i < src.size(); ++i) {
        if (src[i] == '{') {
            ++depth;
        } else if (src[i] == '}') {
            --depth;
            if (depth == 0) {
                return src.substr(open, i - open + 1);
            }
        }
    }
    std::fprintf(stderr, "FAIL: тело %s не закрыто\n", signature.c_str());
    std::exit(1);
}

int pos_of(const std::string &body, const char *needle)
{
    const size_t p = body.find(needle);
    return (p == std::string::npos) ? -1 : static_cast<int>(p);
}

/** Позиция вхождения, которое в теле быть должно. */
int expect_present(const std::string &body, const char *needle, const char *what)
{
    const int p = pos_of(body, needle);
    char msg[512];
    std::snprintf(msg, sizeof(msg), "в теле есть «%s» (%s)", needle, what);
    CHECK(p >= 0, msg);
    return p;
}

/** Порядок: вхождение a раньше b. */
void expect_before(const std::string &body, const char *a, const char *b)
{
    const int pa = expect_present(body, a, "первый из пары");
    const int pb = expect_present(body, b, "второй из пары");
    char msg[512];
    std::snprintf(msg, sizeof(msg), "порядок init: «%s» раньше «%s»", a, b);
    CHECK(pa < pb, msg);
}

std::string main_dir()
{
    /* Путь к firmware/main передаётся макросом при сборке стенда
     * (-DKOUKI_MAIN_DIR=...): тест читает исходники, а не собрать их. */
    const char *dir = KOUKI_MAIN_DIR;
    CHECK(dir != nullptr && dir[0] != '\0', "KOUKI_MAIN_DIR задан (-DKOUKI_MAIN_DIR)");
    return std::string(dir);
}

// ---------------------------------------------------------------------------
// Порядок инициализации: ровно как app_main master (db4bd1b:160-173).
// ---------------------------------------------------------------------------
void test_init_order_matches_master()
{
    const std::string src = strip_comments(read_file(main_dir() + "/kouki_api.cpp"));
    const std::string body = body_of(src, "esp_err_t station_init()");

    /* master: esp_board_init(16000,2,16) -> audio_out -> player -> EXIO -> LCD
     * -> LVGL -> voice -> history -> power -> ui -> консоль -> ota. */
    const int p_board = expect_present(body, "esp_board_init(16000, 2, 16)",
                                       "параметры звука как в master");
    const int p_audio = expect_present(body, "audio_out().init()", "audio_out");
    const int p_player = expect_present(body, "player_init()", "усилитель/громкость");
    const int p_exio = expect_present(body, "EXIO_Init()", "расширитель GPIO");
    const int p_lcd = expect_present(body, "LCD_Init()", "LCD");
    const int p_lvgl = expect_present(body, "LVGL_Init()", "LVGL");
    const int p_voice = expect_present(body, "voice_engine().init(nullptr, nullptr)",
                                       "голосовое ядро");
    const int p_history = expect_present(body, "history().init()", "история");
    const int p_power = expect_present(body, "power().init()", "питание");
    const int p_ui = expect_present(body, "kouki_ui_init()", "UI");
    const int p_console = expect_present(body, "setup().console_init()", "консоль");
    const int p_ota = expect_present(body, "ota().init()", "OTA");

    CHECK(p_board < p_audio, "порядок init: esp_board_init раньше audio_out");
    CHECK(p_audio < p_player, "порядок init: audio_out раньше усилителя");
    CHECK(p_player < p_exio, "порядок init: усилитель раньше EXIO");
    CHECK(p_exio < p_lcd, "порядок init: EXIO раньше LCD (сброс через расширитель)");
    CHECK(p_lcd < p_lvgl, "порядок init: LCD раньше LVGL");
    CHECK(p_lvgl < p_voice, "порядок init: LVGL раньше голосового ядра");
    CHECK(p_voice < p_history, "порядок init: голос раньше истории");
    CHECK(p_history < p_power, "порядок init: история раньше питания");
    CHECK(p_power < p_ui, "порядок init: питание раньше UI");
    CHECK(p_ui < p_console, "порядок init: UI раньше консоли");
    CHECK(p_console < p_ota, "порядок init: консоль раньше OTA");

    /* Аудио-борда раньше экрана — потому что I2C поднимает аудио-борда
     * (в master регресс «LCD до board_init» ловится как чёрный экран). */
    CHECK(p_board < p_exio, "аудио-борда раньше EXIO/LCD: I2C создаётся ей");

    /* OTA инициализируется ДО выбора ветки сети: в master так же. */
    const int p_branch = expect_present(body, "config().is_complete()", "ветка сети");
    CHECK(p_ota < p_branch, "OTA раньше ветки Wi-Fi/SoftAP");

    /* Хвост порядка: SoftAP-портал либо задача сети, затем итоговый лог. */
    expect_present(body, "setup().start()", "SoftAP при неполной конфигурации");
    expect_present(body, "xTaskCreate(wifi_task", "задача сети");
    expect_present(body, "\"kouki.wifi\"", "имя задачи сети как в master");
    expect_present(body, "6144", "стек задачи сети как в master");
    expect_present(body, ", 4,", "приоритет задачи сети как в master");
    expect_present(body, "станция готова: %d с записи на одну фразу",
                   "итоговый лог станции");

    /* Отказы возвращаются первым esp_err_t, а не проглатываются: station_init
     * вызывается из main через ESP_ERROR_CHECK. */
    expect_present(body, "return err;", "ошибка шага возвращается наружу");
}

// ---------------------------------------------------------------------------
// Сборка объектов: одна таблица зависимостей, порядок ссылок корректный.
// ---------------------------------------------------------------------------
void test_station_wiring()
{
    const std::string src = strip_comments(read_file(main_dir() + "/kouki_api.cpp"));
    const std::string body = body_of(src, "Station &station()");

    /* Каждый модуль графа присутствует в Station ровно один раз. */
    const char *factories[] = {
        "config()",      "timekeeper()",  "history()",     "power()",
        "http()",        "audio_out()",   "tone()",        "vad()",
        "wifi()",        "ota()",         "wake()",        "wake_verify()",
        "speech()",      "setup()",       "voice_engine()",
    };
    int prev = -1;
    for (const char *f : factories) {
        const int p = expect_present(body, f, "модуль в объектном графе");
        char msg[256];
        std::snprintf(msg, sizeof(msg), "порядок создания: %s после предыдущего", f);
        CHECK(p > prev, msg);
        prev = p;
    }

    /* Зависимость раньше зависимого — иначе конструктор получит объект,
     * который ещё не создан (в C++ это UB, а не ошибка компиляции). */
    expect_before(body, "config()", "http()");
    expect_before(body, "config()", "wifi()");
    expect_before(body, "wifi()", "ota()");
    expect_before(body, "http()", "ota()");
    expect_before(body, "audio_out()", "tone()");
    expect_before(body, "tone()", "speech()");
    expect_before(body, "vad()", "voice_engine()");
    expect_before(body, "wake()", "voice_engine()");

    /* Экземпляры статические: function-local static, без new/delete. */
    expect_present(body, "static Station s_station", "граф живёт всё время работы");
    CHECK(src.find("new ") == std::string::npos, "в сборке графа нет динамики (new)");
    CHECK(src.find("delete ") == std::string::npos, "в сборке графа нет delete");
}

// ---------------------------------------------------------------------------
// main.c остаётся C-файлом и делает только то, что ДО объектов.
// ---------------------------------------------------------------------------
void test_main_stays_c_entry_point()
{
    const std::string raw_main = read_file(main_dir() + "/main.c");
    const std::string main_src = strip_comments(raw_main);

    /* C, а не C++: нет классов, namespace, ссылок на .hpp (комментарии уже
     * вырезаны — иначе пример кода в шапке файла считался бы нарушением). */
    CHECK(main_src.find("::") == std::string::npos, "main.c не использует C++-синтаксис (::)");
    CHECK(main_src.find(".hpp") == std::string::npos, "main.c не тянет C++-заголовки");
    CHECK(main_src.find("namespace") == std::string::npos, "main.c без namespace");
    CHECK(main_src.find("class ") == std::string::npos, "main.c без классов");

    const std::string body = body_of(main_src, "void app_main(void)");

    /* Порядок: NVS -> BOOT-сброс -> config -> граф -> init -> главный цикл. */
    const int p_nvs = expect_present(body, "nvs_flash_init()", "NVS");
    const int p_boot = expect_present(body, "gpio_config(&boot_cfg)", "кнопка BOOT");
    const int p_cfg = expect_present(body, "kouki_config_init()", "конфигурация");
    const int p_wire = expect_present(body, "kouki_station_wire()", "сборка объектов");
    const int p_init = expect_present(body, "kouki_station_init()", "порядок init");
    const int p_loop = expect_present(body, "lv_timer_handler()", "главный цикл LVGL");

    CHECK(p_nvs < p_cfg, "main.c: NVS раньше конфигурации");
    CHECK(p_boot < p_cfg, "main.c: проверка BOOT раньше чтения конфигурации");
    CHECK(p_cfg < p_wire, "main.c: конфигурация раньше сборки объектов");
    CHECK(p_wire < p_init, "main.c: объекты собираются до инициализации");
    CHECK(p_init < p_loop, "main.c: главный цикл после инициализации");

    /* Отказы не проглатываются: обе точки под ESP_ERROR_CHECK. */
    CHECK(body.find("ESP_ERROR_CHECK(kouki_station_wire())") != std::string::npos,
          "main.c: сборка графа под ESP_ERROR_CHECK");
    CHECK(body.find("ESP_ERROR_CHECK(kouki_station_init())") != std::string::npos,
          "main.c: init под ESP_ERROR_CHECK");
    CHECK(body.find("ESP_ERROR_CHECK(kouki_config_init())") != std::string::npos,
          "main.c: конфигурация под ESP_ERROR_CHECK");

    /* Сброс настроек по удержанию BOOT сохранён (10 с, kouki_config_erase). */
    expect_present(body, "kouki_config_erase()", "сброс настроек кнопкой BOOT");
    expect_present(body, "10000", "удержание BOOT 10 с как в master");

    /* Прежний главный цикл: питание по dt>=50 и пауза 10 мс. */
    expect_present(body, "kouki_power_tick(", "тик питания в главном цикле");
    expect_present(body, "50", "порог dt 50 мс как в master");
    expect_present(body, "vTaskDelay(pdMS_TO_TICKS(10))", "пауза цикла 10 мс");
}

// ---------------------------------------------------------------------------
// C-фасад: одна точка стыка, подписи C-совместимые.
// ---------------------------------------------------------------------------
void test_c_facade()
{
    const std::string hdr = read_file(main_dir() + "/kouki_api.h");
    CHECK(hdr.find("esp_err_t kouki_station_wire(void)") != std::string::npos,
          "kouki_api.h объявляет kouki_station_wire");
    CHECK(hdr.find("esp_err_t kouki_station_init(void)") != std::string::npos,
          "kouki_api.h объявляет kouki_station_init");
    CHECK(hdr.find("extern \"C\"") != std::string::npos, "kouki_api.h с C-linkage");

    const std::string src = strip_comments(read_file(main_dir() + "/kouki_api.cpp"));
    CHECK(src.find("extern \"C\" esp_err_t kouki_station_wire(void)") != std::string::npos,
          "фасад kouki_station_wire определён");
    CHECK(src.find("extern \"C\" esp_err_t kouki_station_init(void)") != std::string::npos,
          "фасад kouki_station_init определён");
    /* C-подписи сверяет компилятор: .cpp -> .hpp -> .h, поэтому достаточно,
     * что цепочка начинается с kouki_api.hpp, а тот включает C-заголовок. */
    const std::string hpp = read_file(main_dir() + "/kouki_api.hpp");
    CHECK(src.find("#include \"kouki_api.hpp\"") != std::string::npos,
          "kouki_api.cpp включает kouki_api.hpp");
    CHECK(hpp.find("#include \"kouki_api.h\"") != std::string::npos,
          "kouki_api.hpp включает C-заголовок (подписи сверяются компилятором)");

    /* Вендорные драйверы: их заголовки без extern "C", поэтому в C++-файле
     * должны быть только локальные C-объявления, а не #include "ST77916.h". */
    CHECK(src.find("#include \"LVGL_Driver.h\"") == std::string::npos,
          "kouki_api.cpp не включает вендорный C-заголовок LVGL_Driver.h");
    CHECK(src.find("extern \"C\" {") != std::string::npos,
          "вендорные EXIO/LCD/LVGL объявлены в extern \"C\"");

    /* Сборка в CMake: новый файл должен быть в SRCS, иначе код не попадёт в образ. */
    const std::string cmake = read_file(main_dir() + "/CMakeLists.txt");
    CHECK(cmake.find("\"./kouki_api.cpp\"") != std::string::npos,
          "kouki_api.cpp в SRCS component main");
    CHECK(cmake.find("\"./main.c\"") != std::string::npos,
          "main.c остаётся C-файлом в SRCS");

    /* Заголовок виден C-компилятору: main.c включает именно его. */
    const std::string main_src = read_file(main_dir() + "/main.c");
    CHECK(main_src.find("#include \"kouki_api.h\"") != std::string::npos,
          "main.c подключает C-заголовок kouki_api.h");
}

// ---------------------------------------------------------------------------
// Логи точки входа: тексты остались прежними (их ждут sim-сценарии).
// ---------------------------------------------------------------------------
void test_log_texts_preserved()
{
    const std::string src = strip_comments(read_file(main_dir() + "/kouki_api.cpp"));
    const char *texts[] = {
        "усилитель динамика включён (GPIO %d)",
        "не удалось поднять усилитель (GPIO %d): %s",
        "громкость выхода: %d %%",
        "кодек не принял громкость %d: %s",
        "сеть готова, сервер %s",
        "без сети: кнопка будет отвечать ошибкой отправки",
        "станция готова: %d с записи на одну фразу",
        "конфигурация готова, подключение к сети",
    };
    for (const char *t : texts) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "лог точки входа сохранён: %s", t);
        CHECK(src.find(t) != std::string::npos, msg);
    }
}

}  // namespace

int main()
{
    test_init_order_matches_master();
    test_station_wiring();
    test_main_stays_c_entry_point();
    test_c_facade();
    test_log_texts_preserved();

    std::printf("=== Station: %d проверок PASS ===\n", g_checks);
    return 0;
}
