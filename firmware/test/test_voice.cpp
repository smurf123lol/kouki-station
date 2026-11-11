// Host-стенд T121 П12: голосовое ядро как класс kouki::Voice.
//
// На хосте (без ESP_PLATFORM) задача записи не поднимается и микрофон не
// читается: так же ведёт себя и master-модуль (весь I2S-путь под #ifdef
// ESP_PLATFORM). Поэтому стенд проверяет то, что видно и без платы:
//   * ИНВАРИАНТ «один владелец I2S» по исходникам: esp_get_feed_data есть
//     ровно в двух местах app_voice.cpp (read_mic и диагностика) и ни в одном
//     другом файле firmware/main — детектор wake получает PCM из FSM;
//   * сборку объектов, повторную инициализацию и регистрацию состояний;
//   * сессии, запись через кнопку, событие смены состояния через колбэк;
//   * равенство C-фасада и методов класса.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include "app_voice.h"
#include "app_voice.hpp"

static int g_checks = 0;

#define CHECK(cond, msg) do {                                                 \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        std::exit(1);                                                         \
    }                                                                         \
} while (0)

namespace {

/** Колбэк смены состояния: считает вызовы и запоминает последнее состояние. */
struct EventSink {
    int calls = 0;
    kouki_voice_state_t last = KOUKI_VOICE_IDLE;
    int recording_seen = 0;
};

void event_cb(kouki_voice_state_t state, const kouki_voice_result_t *result, void *ctx)
{
    EventSink *sink = static_cast<EventSink *>(ctx);
    sink->calls++;
    sink->last = state;
    if (state == KOUKI_VOICE_RECORDING) {
        sink->recording_seen++;
    }
    (void)result;
}

/** Читает файл целиком; пустая строка, если файла нет. */
std::string read_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/** Все вхождения подстроки. */
int count_occurrences(const std::string &haystack, const std::string &needle)
{
    int n = 0;
    if (needle.empty()) {
        return 0;
    }
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        n++;
    }
    return n;
}

/**
 * Вырезает строчные и блочные комментарии, сохраняя переводы строк.
 *
 * Нужно для инварианта I2S: сам запрет описан словами в шапках app_voice.hpp и
 * app_wake.hpp («не зовёт esp_get_feed_data/esp_codec_dev_read»), и без этой
 * чистки сканирование заголовков дало бы ложное срабатывание на собственном
 * комментарии. Тело строковых литералов сохраняется как есть.
 */
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

/**
 * Каталоги firmware/main, где чтение микрофона разрешено.
 *
 * drivers/ — это board-код: esp_board_init()/esp_get_feed_data() и кодек живут
 * там по устройству (firmware/main/drivers/README.md). test/ — не прошивка.
 */
bool is_mic_reader_exempt(const std::filesystem::path &path, const std::filesystem::path &main_dir)
{
    const std::filesystem::path rel = std::filesystem::relative(path, main_dir);
    for (const auto &part : rel) {
        const std::string name = part.string();
        if (name == "drivers" || name == "test") {
            return true;
        }
    }
    return false;
}

/**
 * ИНВАРИАНТ I2S (AGENTS.md п.1): микрофон читает только голосовое ядро.
 *
 * Проверка идёт по исходникам, а не по рантайму: на хосте задача записи не
 * поднимается, поэтому счётчик вызовов стаба ничего не докажет. Зато по коду
 * видно, что второй читатель не появился ни в одном модуле.
 *
 * Сканируются .c/.cpp И .h/.hpp (находка ревью П12: читатель, спрятанный в
 * inline-функции заголовка, старый тест не видел), комментарии вырезаются.
 * Проверяются ВСЕ три способа чтения: esp_get_feed_data (board-API),
 * esp_codec_dev_read (кодек напрямую) и i2s_channel_read (канал IDF).
 */
void test_i2s_owner_invariant()
{
    std::printf("[TEST] 1. Инвариант I2S: единственный читатель микрофона...\n");

    const std::filesystem::path main_dir(KOUKI_MAIN_DIR);
    CHECK(std::filesystem::exists(main_dir), "каталог firmware/main доступен стенду");

    std::vector<std::filesystem::path> sources;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(main_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext == ".cpp" || ext == ".c" || ext == ".hpp" || ext == ".h") {
            sources.push_back(entry.path());
        }
    }
    CHECK(!sources.empty(), "исходники firmware/main найдены");

    // Все способы прочитать микрофон, которые существуют в IDF. Если появится
    // четвёртый (например, i2s_channel_read в обход board-API), его надо
    // добавить сюда же — иначе инвариант снова станет частичным.
    const std::vector<std::string> readers = {
        "esp_get_feed_data(",
        "esp_codec_dev_read(",
        "i2s_channel_read(",
    };

    const std::string voice_src = read_file(main_dir / "app_voice.cpp");
    CHECK(!voice_src.empty(), "app_voice.cpp читается");
    CHECK(count_occurrences(voice_src, "esp_get_feed_data(") == 2,
          "в app_voice.cpp ровно два чтения микрофона (запись + диагностика)");
    CHECK(count_occurrences(voice_src, "esp_codec_dev_read(") == 0,
          "голосовое ядро не читает кодек в обход board-API");
    CHECK(count_occurrences(voice_src, "i2s_channel_read(") == 0,
          "голосовое ядро не читает I2S-канал напрямую");

    const std::string wake_src = read_file(main_dir / "app_wake.cpp");
    CHECK(!wake_src.empty(), "app_wake.cpp читается");
    for (const auto &reader : readers) {
        CHECK(count_occurrences(wake_src, reader) == 0,
              "детектор wake не читает микрофон сам (ни одним из трёх способов)");
    }

    // Ни в одном другом файле прошивки (включая заголовки) чтения микрофона
    // быть не должно. По одной проверке на файл: нарушение сразу называет и
    // файл, и все способы чтения, которые в нём встретились.
    int scanned = 0;
    for (const auto &path : sources) {
        if (path.filename() == "app_voice.cpp" || is_mic_reader_exempt(path, main_dir)) {
            continue;
        }
        scanned++;
        const std::string code = strip_comments(read_file(path));
        std::string found;
        for (const auto &reader : readers) {
            const int n = count_occurrences(code, reader);
            if (n > 0) {
                found += " " + reader + " x" + std::to_string(n);
            }
        }
        const std::string rel = std::filesystem::relative(path, main_dir).string();
        const std::string msg = "чтение микрофона только в app_voice.cpp, а в " + rel + " есть:" +
                                (found.empty() ? std::string(" нет") : found);
        CHECK(found.empty(), msg.c_str());
    }
    CHECK(scanned > 0, "файлы для проверки инварианта найдены");

    // Детектор получает PCM от FSM: IdleState отдаёт чанк из record_task.
    const std::string idle_src = read_file(main_dir / "fsm/states/idle_state.cpp");
    CHECK(!idle_src.empty(), "idle_state.cpp читается");
    CHECK(idle_src.find("kouki_wake_feed(") != std::string::npos,
          "IdleState кормит детектор готовым PCM из задачи записи");

    // Задача записи — ровно одна и с прежним именем/приоритетом.
    CHECK(count_occurrences(voice_src, "xTaskCreatePinnedToCore") == 1,
          "задача записи создаётся в одном месте");
    CHECK(voice_src.find("\"kouki.record\"") != std::string::npos, "имя задачи записи прежнее");
}

void test_facade_before_init()
{
    std::printf("[TEST] 2. Состояние до инициализации и фасад 1:1...\n");

    CHECK(kouki_voice_get_state() == KOUKI_VOICE_IDLE, "до init состояние IDLE");
    CHECK(kouki::voice_engine().state() == KOUKI_VOICE_IDLE, "класс тоже видит IDLE");
    CHECK(!kouki_voice_is_recording(), "до init запись не идёт");
    CHECK(!kouki::voice_engine().is_recording(), "класс тоже не в записи");

    kouki_voice_metrics_t m{};
    kouki_voice_get_metrics(&m);
    CHECK(!m.is_recording, "метрики: записи нет");

    CHECK(kouki_voice_get_session() == KOUKI_SESSION_TASK, "сессия по умолчанию — TASK");
    CHECK(kouki::voice_engine().session() == KOUKI_SESSION_TASK, "класс видит ту же сессию");
}

void test_init_and_events()
{
    std::printf("[TEST] 3. Инициализация, колбэк смены состояния, повторный init...\n");

    EventSink sink;
    CHECK(kouki_voice_init(event_cb, &sink) == ESP_OK, "init() -> ESP_OK");
    // master: init() только регистрирует колбэк, переходов нет — событий тоже
    // (IdleState::on_enter ничего не публикует).
    CHECK(sink.calls == 0, "init() не публикует событие смены состояния");
    CHECK(kouki_voice_get_state() == KOUKI_VOICE_IDLE, "после init состояние IDLE");

    // Повторная инициализация: прежнее поведение — переинициализация
    // менеджера и новый буфер, ошибки нет (master 1:1).
    EventSink sink2;
    CHECK(kouki_voice_init(event_cb, &sink2) == ESP_OK, "повторный init() -> ESP_OK");
    CHECK(kouki_voice_get_state() == KOUKI_VOICE_IDLE, "после повторного init состояние IDLE");
    CHECK(kouki::voice_engine().state() == KOUKI_VOICE_IDLE, "класс видит то же");

    // Буфер под запись выделен: контекст знает ёмкость.
    CHECK(kouki::voice_engine().state() == KOUKI_VOICE_IDLE, "состояние стабильно после init");
}

void test_sessions()
{
    std::printf("[TEST] 4. Сессии записи: TASK/ASK и запрет во время записи...\n");

    CHECK(kouki_voice_set_session(KOUKI_SESSION_ASK) == ESP_OK, "сессия ASK выбирается в IDLE");
    CHECK(kouki_voice_get_session() == KOUKI_SESSION_ASK, "фасад видит ASK");
    CHECK(kouki::voice_engine().session() == KOUKI_SESSION_ASK, "класс видит ASK");

    CHECK(kouki_voice_set_session(KOUKI_SESSION_TASK) == ESP_OK, "сессия TASK выбирается");
    CHECK(kouki_voice_get_session() == KOUKI_SESSION_TASK, "сессия вернулась к TASK");
}

void test_button_recording()
{
    std::printf("[TEST] 5. Запись кнопкой: старт, стоп, состояние, колбэк...\n");

    EventSink sink;
    CHECK(kouki_voice_init(event_cb, &sink) == ESP_OK, "init() перед записью");
    const int before = sink.calls;

    CHECK(kouki_voice_start_recording() == ESP_OK, "старт записи из IDLE");
    CHECK(kouki_voice_get_state() == KOUKI_VOICE_RECORDING, "состояние RECORDING");
    CHECK(kouki_voice_is_recording(), "фасад: запись идёт");
    CHECK(kouki::voice_engine().is_recording(), "класс: запись идёт");
    CHECK(sink.calls > before, "колбэк вызван на переходе");
    CHECK(sink.last == KOUKI_VOICE_RECORDING, "колбэк сообщил RECORDING");
    CHECK(sink.recording_seen >= 1, "событие записи дошло");

    // Повторный старт во время записи ничего не меняет (master 1:1).
    CHECK(kouki_voice_start_recording() == ESP_OK, "повторный старт ничего не ломает");
    CHECK(kouki_voice_get_state() == KOUKI_VOICE_RECORDING, "всё ещё RECORDING");

    // Сессию во время записи менять нельзя.
    CHECK(kouki_voice_set_session(KOUKI_SESSION_ASK) == ESP_ERR_INVALID_STATE,
          "смена сессии во время записи отвергнута");

    // Остановка с кнопки только выставляет stop_requested: состояние
    // остаётся RECORDING, пока задача записи не шагнёт автоматом (master 1:1 —
    // отправку затевает только владелец буфера).
    CHECK(kouki_voice_stop_and_send() == ESP_OK, "стоп-и-отправка -> ESP_OK");
    CHECK(kouki_voice_get_state() == KOUKI_VOICE_RECORDING,
          "до шага задачи записи состояние остаётся RECORDING");
    CHECK(kouki::voice_engine().state() == KOUKI_VOICE_RECORDING, "класс видит то же");

    // Повторный стоп ничего не ломает.
    CHECK(kouki_voice_stop_and_send() == ESP_OK, "повторный стоп -> ESP_OK");
}

void test_snapshot_and_print()
{
    std::printf("[TEST] 6. Снимок состояния, диагностика, dismiss_error...\n");

    kouki_voice_state_t state = KOUKI_VOICE_ERROR;
    kouki_voice_result_t result{};
    CHECK(kouki_voice_get_snapshot(&state, &result), "снимок состояния доступен");
    CHECK(state == kouki_voice_get_state(), "снимок согласован с get_state()");

    // print_state печатает разбор FSM — вывод не проверяем, но вызов безопасен.
    kouki_voice_print_state();
    kouki::voice_engine().print_state();

    // dismiss_error вне ERROR — ESP_ERR_INVALID_STATE (master 1:1).
    CHECK(kouki_voice_dismiss_error() == ESP_ERR_INVALID_STATE,
          "dismiss_error вне ERROR -> ESP_ERR_INVALID_STATE");
    CHECK(kouki::voice_engine().dismiss_error() == ESP_ERR_INVALID_STATE,
          "то же через класс");
}

void test_debug_levels_noop_on_host()
{
    std::printf("[TEST] 7. Диагностика уровней на хосте — безопасный no-op...\n");

    // На хосте I2S нет: вызов обязан быть no-op и не падать.
    kouki_voice_debug_levels();
    kouki::voice_engine().debug_levels();
    CHECK(kouki_voice_get_state() == kouki::voice_engine().state(),
          "диагностика не меняет состояние FSM");
}

}  // namespace

int main()
{
    std::printf("=== Voice: host-стенд (T121 П12) ===\n");
    test_i2s_owner_invariant();
    test_facade_before_init();
    test_init_and_events();
    test_sessions();
    test_button_recording();
    test_snapshot_and_print();
    test_debug_levels_noop_on_host();
    std::printf("=== Voice: %d проверок PASS ===\n", g_checks);
    return 0;
}
