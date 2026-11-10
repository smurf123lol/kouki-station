// Host-стенд T121 П9: классы kouki::Wake и kouki::WakeVerify и их C-фасады.
//
// Плата не нужна: мьютeкс/задача FreeRTOS, esp-dl rfft, heap_caps и время
// заменены моками (support/wake), HTTP-клиент ASR-проверки — общим моком
// support/http_mock. Что проверяется:
//   * детектор: включение через NVS wake_en, повторный init, deinit/reinit,
//     ESP_ERR_NO_MEM на FFT и на буферах (без течи буферов), mute 2.5 с после
//     init и явный mute(), сброс буферов, кольцо предзаписи и его переполнение;
//   * распознавание считается по-настоящему: реальные фикстуры
//     sim/fixtures/mic/*.wav идут через настоящую модель microWakeWord
//     (заменён только rfft — наивный DFT с тем же форматом выхода);
//   * ASR-подтверждение: WAV-заголовок и тело POST /api/transcribe, Bearer,
//     разбор transcript, ACCEPTED/REJECTED/UNAVAILABLE по статусам, отказ
//     второго запуска при PENDING, сброс состояния только после завершения;
//   * матчинг «эй гин» по вариантам распознавания Whisper.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app_config.h"
#include "app_wake.h"
#include "app_wake.hpp"
#include "app_wake_verify.h"
#include "app_wake_verify.hpp"
#include "esp_http_client.h"
#include "nvs_mock.h"
#include "wake_mock.h"

/* Тот же инвариант, но уже на стенде: если кто-то снимет alignas с буферов
 * класса, host-сборка это заметит (в отличие от поведения DFT-мока). */
static_assert(alignof(kouki::Wake) >= 16, "буферы Wake должны быть выровнены по 16 байт: dl_rfft_f32_run на S3 — SIMD");

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

// Размер чанка, которым record_task кормит детектор (fsm/voice_context.hpp:18).
constexpr int kChunk = 1024;

/** Каталог репозитория от __FILE__ (firmware/test/test_wake.cpp). */
std::string repo_root()
{
    std::string test_file(__FILE__);
    const size_t slash = test_file.find_last_of('/');
    const std::string test_dir = slash == std::string::npos ? "." : test_file.substr(0, slash);
    return test_dir + "/../..";
}

/** Читает PCM из WAV-фикстуры (16 кГц, моно, 16 бит, заголовок 44 байта). */
std::vector<int16_t> load_wav(const std::string &path)
{
    std::FILE *fp = std::fopen(path.c_str(), "rb");
    CHECK(fp != nullptr, "фикстура открывается");
    std::vector<unsigned char> raw(65536);
    const size_t read = std::fread(raw.data(), 1, raw.size(), fp);
    std::fclose(fp);
    CHECK(read > 44, "фикстура не пустая");
    raw.resize(read);
    CHECK(std::memcmp(raw.data(), "RIFF", 4) == 0, "фикстура — RIFF");
    CHECK(std::memcmp(raw.data() + 8, "WAVE", 4) == 0, "фикстура — WAVE");

    std::vector<int16_t> pcm;
    pcm.reserve((read - 44) / 2);
    for (size_t i = 44; i + 1 < read; i += 2) {
        pcm.push_back(static_cast<int16_t>(raw[i] | (raw[i + 1] << 8)));
    }
    return pcm;
}

/** Кормит детектор чанками как record_task: по 1024 сэмпла. */
int feed_chunks(const std::vector<int16_t> &pcm)
{
    int hits = 0;
    size_t pos = 0;
    while (pos < pcm.size()) {
        size_t n = pcm.size() - pos;
        if (n > static_cast<size_t>(kChunk)) {
            n = static_cast<size_t>(kChunk);
        }
        if (kouki_wake_feed(pcm.data() + pos, static_cast<int>(n))) {
            hits++;
        }
        pos += n;
    }
    return hits;
}

/** База «часов» моков: растёт от теста к тесту, чтобы дедлайны прошлых
 *  тестов (mute_until_us живёт в классе) оставались в прошлом. */
long long g_clock_base_us = 0;

/** Общий сброс перед каждым тестом: чистая NVS, конфигурация, моки, детектор. */
void reset_all(bool wake_enabled = true, uint8_t threshold = 65)
{
    /* Классы — единственные экземпляры на процесс (фабрики kouki::wake() и
     * kouki::wake_verify()), поэтому состояние от прошлого теста снимаем
     * явно: deinit() детектора и reset() проверки. */
    kouki_wake_deinit();
    kouki_wake_verify_reset();

    nvs_mock_reset();
    nvs_mock_seed_u8(kNs, "cfg_ver", KOUKI_CONFIG_VERSION);
    nvs_mock_seed_str(kNs, "srv_url", "http://192.168.1.20:5300");
    nvs_mock_seed_str(kNs, "srv_token", "0123456789abcdef");
    nvs_mock_seed_u8(kNs, "wake_en", wake_enabled ? 1 : 0);
    nvs_mock_seed_u8(kNs, "wake_th", threshold);
    CHECK(kouki_config_init() == ESP_OK, "init() конфигурации -> ESP_OK");

    wake_mock_reset();
    http_mock_reset();

    /* mute_until_us — член класса и переживает deinit(), а часы моков
     * сбрасываются в ноль. База времени растёт от теста к тесту, поэтому
     * дедлайн глушения прошлого теста гарантированно в прошлом; затем глушим
     * на 1 мс и прокручиваем ещё — окно глушения закрыто. */
    g_clock_base_us += 3600LL * 1000 * 1000;
    wake_mock_advance_us(g_clock_base_us);
    kouki_wake_mute(1);
    wake_mock_advance_us(10 * 1000 * 1000);
}

/** Тишина: нули столько сэмплов. */
std::vector<int16_t> make_silence(size_t samples)
{
    return std::vector<int16_t>(samples, 0);
}

/** Тихое монотонное заполнение — ниже энергетического порога 500. */
std::vector<int16_t> make_quiet(size_t samples)
{
    std::vector<int16_t> pcm(samples);
    for (size_t i = 0; i < samples; i++) {
        pcm[i] = static_cast<int16_t>((i % 8) - 4);
    }
    return pcm;
}

}  // namespace

// ------------------------------------------------- инициализация детектора ---

static void test_init_disabled()
{
    std::printf("[TEST] 1. wake_en=0: init -> ESP_ERR_NOT_SUPPORTED, детектор не готов...\n");
    reset_all(false);

    CHECK(kouki_wake_init() == ESP_ERR_NOT_SUPPORTED, "init при wake_en=0 -> NOT_SUPPORTED");
    CHECK(!kouki_wake_is_ready(), "детектор не готов");
    CHECK(wake_mock_rfft_inits() == 0, "FFT не создавали, если фича выключена");
    CHECK(wake_mock_heap_caps_calls() == 0, "буферы не выделяли, если фича выключена");
    CHECK(kouki_wake_feed(make_silence(64).data(), 64) == false, "feed при выключенной фиче -> false");
    CHECK(std::strcmp(kouki_wake_model(), "эй гин") == 0, "имя модели прежнее");
}

static void test_init_and_reinit()
{
    std::printf("[TEST] 2. init: FFT + три буфера, повторный init — без работы...\n");
    reset_all();

    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    CHECK(kouki_wake_is_ready(), "детектор готов");
    CHECK(wake_mock_rfft_inits() == 1, "FFT-инстанс создан один раз");
    CHECK(wake_mock_heap_caps_calls() == 3, "три буфера: спектрограмма, нормализация, арена");
    CHECK(wake_mock_heap_caps_spiram_calls() == 3, "все три сначала просят PSRAM, как в образе");
    /* Мьютекс состояния создаётся лениво и живёт столько же, сколько класс:
     * reset_all() уже вызвал deinit() до сброса счётчиков моков, поэтому здесь
     * ноль созданий означает «мьютекс переиспользован, а не пересоздан». */
    CHECK(wake_mock_semaphore_creates() == 0, "мьютекс состояния не пересоздаётся на init");
    CHECK(wake_mock_semaphore_takes() > 0, "init берёт мьютекс состояния");

    CHECK(kouki_wake_init() == ESP_OK, "повторный init -> ESP_OK");
    CHECK(wake_mock_rfft_inits() == 1, "повторный init не создаёт второй FFT");
    CHECK(wake_mock_heap_caps_calls() == 3, "повторный init не выделяет буферы заново");

    kouki_wake_deinit();
    CHECK(!kouki_wake_is_ready(), "после deinit детектор не готов");
    CHECK(kouki_wake_init() == ESP_OK, "init после deinit разрешён");
    CHECK(wake_mock_rfft_inits() == 2, "после deinit FFT создаётся заново");
    CHECK(wake_mock_semaphore_creates() == 0, "deinit/init не пересоздают мьютекс");
}

static void test_init_no_mem()
{
    std::printf("[TEST] 3. init: нехватка FFT/буферов -> ESP_ERR_NO_MEM, повторный init работает...\n");
    reset_all();

    wake_mock_fail_next_rfft_init();
    CHECK(kouki_wake_init() == ESP_ERR_NO_MEM, "нет FFT-инстанса -> NO_MEM");
    CHECK(!kouki_wake_is_ready(), "после ошибки детектор не готов");

    /* Буфер спектрограммы: PSRAM и INTERNAL — два вызова подряд. */
    wake_mock_fail_heap_caps_malloc(2);
    CHECK(kouki_wake_init() == ESP_ERR_NO_MEM, "нет памяти под спектрограмму -> NO_MEM");

    /* Арена активаций — третий по счёту буфер; первые два должны освободиться. */
    wake_mock_fail_heap_caps_malloc(0);
    CHECK(kouki_wake_init() == ESP_OK, "после освобождения буферов init проходит");
}

static void test_mute_after_init()
{
    std::printf("[TEST] 4. init глушит детектор на 2.5 с (щелчки микрофона)...\n");
    reset_all();
    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    kouki_wake_reset();

    const std::vector<int16_t> pcm = make_quiet(4096);
    CHECK(!kouki_wake_feed(pcm.data(), static_cast<int>(pcm.size())), "в окне mute хита нет");
    CHECK(wake_mock_rfft_runs() == 0, "в окне mute FFT не считает");

    wake_mock_advance_us(2600 * 1000);
    kouki_wake_feed(pcm.data(), static_cast<int>(pcm.size()));
    CHECK(wake_mock_rfft_runs() > 0, "после окна mute звук снова обрабатывается");
}

static void test_mute_and_reset_facade()
{
    std::printf("[TEST] 5. mute(): окно глушения, reset() не ломает готовность...\n");
    reset_all();
    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    /* mute_until_us — абсолютное время: окно глушения от init() (2.5 с)
     * прокручиваем, а не обнуляем часы. */
    wake_mock_advance_us(5 * 1000 * 1000);
    kouki_wake_reset();

    kouki_wake_mute(1000);
    const std::vector<int16_t> pcm = make_quiet(2048);
    CHECK(!kouki_wake_feed(pcm.data(), static_cast<int>(pcm.size())), "в окне mute звук не обрабатывают");
    const int runs_before = wake_mock_rfft_runs();

    wake_mock_advance_us(1100 * 1000);
    kouki_wake_feed(pcm.data(), static_cast<int>(pcm.size()));
    CHECK(wake_mock_rfft_runs() > runs_before, "после окна mute FFT снова считает");

    kouki_wake_reset();
    CHECK(kouki_wake_is_ready(), "reset() не снимает готовность");
    CHECK(kouki_wake_feed(nullptr, 10) == false, "пустой PCM -> false");
    CHECK(kouki_wake_feed(pcm.data(), 0) == false, "нулевая длина -> false");
}

static void test_preroll_ring()
{
    std::printf("[TEST] 6. кольцо предзаписи: порядок, переполнение, очистка...\n");
    reset_all();
    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    /* mute_until_us — абсолютное время: окно глушения от init() (2.5 с)
     * прокручиваем, а не обнуляем часы. */
    wake_mock_advance_us(5 * 1000 * 1000);
    kouki_wake_reset();

    /* Кольцо предзаписи переживает и reset(), и deinit() — как в образе
     * (wake_reset_locked() его не трогает). Поэтому чистим явно. */
    kouki_wake_preroll_clear();
    std::vector<int16_t> dst(20000, -1);
    CHECK(kouki_wake_get_preroll(dst.data(), static_cast<int>(dst.size())) == 0, "пустое кольцо -> 0");

    std::vector<int16_t> first(1000);
    for (size_t i = 0; i < first.size(); i++) {
        first[i] = static_cast<int16_t>(i);
    }
    kouki_wake_feed(first.data(), static_cast<int>(first.size()));
    CHECK(kouki_wake_get_preroll(dst.data(), 5000) == 1000, "в кольце 1000 сэмплов");
    CHECK(dst[0] == 0 && dst[999] == 999, "порядок хронологический");

    /* Второй кусок: кольцо продолжается, читаем только хвост. */
    std::vector<int16_t> second(500);
    for (size_t i = 0; i < second.size(); i++) {
        second[i] = static_cast<int16_t>(10000 + i);
    }
    kouki_wake_feed(second.data(), static_cast<int>(second.size()));
    CHECK(kouki_wake_get_preroll(dst.data(), 300) == 300, "просят 300 — отдают 300");
    CHECK(dst[0] == 10000 + 200 && dst[299] == 10000 + 499, "вернули последние 300 сэмплов по порядку");

    /* Переполнение: 20000 сэмплов при кольце 16000 — остаются последние 16000. */
    std::vector<int16_t> big(20000);
    for (size_t i = 0; i < big.size(); i++) {
        big[i] = static_cast<int16_t>(i % 30000);
    }
    kouki_wake_feed(big.data(), static_cast<int>(big.size()));
    CHECK(kouki_wake_get_preroll(dst.data(), static_cast<int>(dst.size())) == 16000,
          "кольцо не больше 16000 сэмплов");
    CHECK(dst[0] == static_cast<int16_t>(19999 - 15999) && dst[15999] == static_cast<int16_t>(19999),
          "в кольце последние 16000 сэмплов");

    kouki_wake_preroll_clear();
    CHECK(kouki_wake_get_preroll(dst.data(), static_cast<int>(dst.size())) == 0, "после clear кольцо пусто");
}

static void test_energy_filter()
{
    std::printf("[TEST] 7. энергетический фильтр: тишина не даёт хита...\n");
    reset_all();
    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    /* mute_until_us — абсолютное время: окно глушения от init() (2.5 с)
     * прокручиваем, а не обнуляем часы. */
    wake_mock_advance_us(5 * 1000 * 1000);
    kouki_wake_reset();

    const std::vector<int16_t> silence = make_silence(32000);
    CHECK(feed_chunks(silence) == 0, "на нулях хита нет");
    CHECK(wake_mock_rfft_runs() > 0, "FFT при этом считается (фильтр — по пикам, не по FFT)");

    const std::vector<int16_t> quiet = make_quiet(32000);
    CHECK(feed_chunks(quiet) == 0, "на амплитуде ниже 500 хита нет");
}

// ------------------------------------- распознавание на реальных фикстурах ---

static void test_fixture_wake_word()
{
    std::printf("[TEST] 8. фикстура ey_gin.wav: детектор срабатывает...\n");
    reset_all();
    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    /* mute_until_us — абсолютное время: окно глушения от init() (2.5 с)
     * прокручиваем, а не обнуляем часы. */
    wake_mock_advance_us(5 * 1000 * 1000);
    kouki_wake_reset();

    const std::vector<int16_t> pcm = load_wav(repo_root() + "/sim/fixtures/mic/ey_gin.wav");
    CHECK(pcm.size() > 16000, "фикстура длиннее секунды");
    const int hits = feed_chunks(pcm);
    CHECK(hits == 1, "на реальной записи «эй гин» ровно один хит");
    CHECK(wake_mock_rfft_runs() > 100, "FFT считался по-настоящему (инференс шёл)");

    /* Хит сбрасывает буферы: кольцо предзаписи пусто, детектор готов дальше. */
    CHECK(kouki_wake_is_ready(), "после хита детектор остаётся готовым");
}

static void test_fixture_threshold()
{
    std::printf("[TEST] 9. порог берётся из Config: 65 молчит, 10 ловит постороннюю речь...\n");
    reset_all(true, 65);
    CHECK(kouki_wake_init() == ESP_OK, "init с порогом 65 -> ESP_OK");
    /* mute_until_us — абсолютное время: окно глушения от init() (2.5 с)
     * прокручиваем, а не обнуляем часы. */
    wake_mock_advance_us(5 * 1000 * 1000);
    kouki_wake_reset();
    CHECK(kouki_config_get_wake_threshold() == 65, "порог из NVS прочитан");

    std::vector<int16_t> bystander = load_wav(repo_root() + "/sim/fixtures/mic/bystander_speech.wav");
    CHECK(feed_chunks(bystander) == 0, "на посторонней речи при пороге 65 хита нет");

    /* Порог меняется на ходу — следующий проход должен поймать ложный хит. */
    CHECK(kouki_config_set_wake_threshold(10) == ESP_OK, "порог 10 принят");
    /* mute_until_us — абсолютное время: окно глушения от init() (2.5 с)
     * прокручиваем, а не обнуляем часы. */
    wake_mock_advance_us(5 * 1000 * 1000);
    kouki_wake_reset();
    CHECK(feed_chunks(bystander) > 0, "при пороге 10 та же речь даёт ложный хит");

    CHECK(kouki_config_set_wake_threshold(5) == ESP_ERR_INVALID_ARG, "порог вне 10..95 отбивается");
}

static void test_ready_follows_config()
{
    std::printf("[TEST] 10. is_ready учитывает выключенный в конфигурации wake_en...\n");
    reset_all();
    CHECK(kouki_wake_init() == ESP_OK, "init -> ESP_OK");
    CHECK(kouki_wake_is_ready(), "готов");

    CHECK(kouki_config_set_wake_enabled(false) == ESP_OK, "выключили фичу на ходу");
    CHECK(!kouki_wake_is_ready(), "готовность снята без deinit");
    const std::vector<int16_t> pcm = make_quiet(2048);
    wake_mock_advance_us(5 * 1000 * 1000);
    CHECK(!kouki_wake_feed(pcm.data(), static_cast<int>(pcm.size())), "feed при wake_en=0 -> false");

    CHECK(kouki_config_set_wake_enabled(true) == ESP_OK, "включили обратно");
    CHECK(kouki_wake_is_ready(), "готовность вернулась");
}

// --------------------------------------------------- ASR-подтверждение ------

static void test_matches()
{
    std::printf("[TEST] 11. матчинг «эй гин» по вариантам Whisper...\n");

    /* Матчинг 1:1 с master: сравнение по подстроке, регистрозависимое
     * (strstr), поэтому «Эй, Дин!» из замера Whisper в верхнем регистре
     * мастер НЕ принимал — поведение сохраняем, а не «улучшаем». */
    CHECK(kouki_wake_verify_matches("эй гин"), "точное совпадение");
    CHECK(kouki_wake_verify_matches("джин"), "слово семейства «джин»");
    CHECK(kouki_wake_verify_matches("Джин, включи свет"), "слово в начале фразы");
    CHECK(kouki_wake_verify_matches("Пейдем!"), "короткое обращение с префиксом «пей»");
    CHECK(kouki_wake_verify_matches("эдит!"), "короткое обращение с префиксом «эд»");
    CHECK(kouki_wake_verify_matches("эй гин включи свет в комнате"), "слово в длинной фразе ловится");

    CHECK(!kouki_wake_verify_matches(""), "пустая строка -> не совпало");
    CHECK(!kouki_wake_verify_matches(nullptr), "NULL -> не совпало");
    CHECK(!kouki_wake_verify_matches("Странно работает."), "посторонняя речь -> не совпало");
    CHECK(!kouki_wake_verify_matches("включи свет в комнате пожалуйста"), "длинная фраза без слова -> нет");
    CHECK(!kouki_wake_verify_matches("Эй, Дин!"), "верхний регистр мастер не ловит (регресс не вводим)");
    CHECK(!kouki_wake_verify_matches("ГИН"), "«ГИН» капсом мастер не ловит");
}

static void test_verify_start_args()
{
    std::printf("[TEST] 12. start(): пустой PCM / повторный запуск при PENDING...\n");
    reset_all();

    const std::vector<int16_t> pcm = make_quiet(1600);
    CHECK(kouki_wake_verify_start(nullptr, 10) == ESP_ERR_INVALID_ARG, "NULL PCM -> INVALID_ARG");
    CHECK(kouki_wake_verify_start(pcm.data(), 0) == ESP_ERR_INVALID_ARG, "нулевая длина -> INVALID_ARG");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_IDLE, "состояние осталось IDLE");

    http_mock_set_response(200, "{\"transcript\": \"эй гин\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_PENDING, "состояние PENDING");
    CHECK(wake_mock_task_creates() == 1, "задача создана");
    CHECK(wake_mock_last_task_stack() == 8192, "стек 8192, как в образе");
    CHECK(wake_mock_last_task_priority() == 1, "приоритет tskIDLE_PRIORITY+1");
    CHECK(std::strcmp(wake_mock_last_task_name(), "wake_verify") == 0, "имя задачи прежнее");

    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_ERR_INVALID_STATE,
          "второй запуск при PENDING -> INVALID_STATE");

    /* reset() не трогает состояние, пока задача в полёте. */
    kouki_wake_verify_reset();
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_PENDING, "reset при PENDING — no-op");

    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_ACCEPTED, "ответ «эй гин» -> ACCEPTED");
    CHECK(std::strcmp(kouki_wake_verify_last_transcript(), "эй гин") == 0, "текст сохранён");

    kouki_wake_verify_reset();
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_IDLE, "reset после завершения -> IDLE");
    CHECK(std::strcmp(kouki_wake_verify_last_transcript(), "") == 0, "reset чистит текст");
}

static void test_verify_request_shape()
{
    std::printf("[TEST] 13. POST /api/transcribe: URL, WAV-заголовок, Bearer...\n");
    reset_all();

    const std::vector<int16_t> pcm(400, 1000);
    http_mock_set_response(200, "{\"transcript\": \"Пейдем!\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_ACCEPTED, "«Пейдем!» -> ACCEPTED");

    const esp_http_client_config_t *cfg = http_mock_last_config();
    CHECK(cfg != nullptr, "клиент создавали");
    CHECK(cfg->method == HTTP_METHOD_POST, "метод POST");
    CHECK(std::strcmp(cfg->url, "http://192.168.1.20:5300/api/transcribe") == 0, "URL собран из srv_url");
    CHECK(cfg->timeout_ms == 4000, "таймаут из CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS");
    CHECK(cfg->buffer_size == 4096, "буфер прежний");

    const char *ctype = http_mock_header("Content-Type");
    CHECK(ctype != nullptr && std::strcmp(ctype, "audio/wav") == 0, "Content-Type: audio/wav");
    const char *auth = http_mock_header("Authorization");
    CHECK(auth != nullptr && std::strcmp(auth, "Bearer 0123456789abcdef") == 0, "Authorization: Bearer токен");

    /* Тело — WAV: 44 байта заголовка + 2 байта на сэмпл. */
    CHECK(http_mock_written_bytes() == 44 + 400 * 2, "отправлено ровно 44 + 2*N байт");
    const char *body = http_mock_written_body();
    CHECK(body != nullptr, "тело записано");
    CHECK(std::memcmp(body, "RIFF", 4) == 0, "RIFF на месте");
    CHECK(std::memcmp(body + 8, "WAVE", 4) == 0, "WAVE на месте");
    CHECK(std::memcmp(body + 12, "fmt ", 4) == 0, "fmt на месте");
    const unsigned char *b = reinterpret_cast<const unsigned char *>(body);
    CHECK(b[20] == 1, "PCM");
    CHECK(b[22] == 1, "моно");
    CHECK(b[24] == 0x80 && b[25] == 0x3E, "частота 16000");
    CHECK(b[34] == 16, "16 бит на сэмпл");
    CHECK(std::memcmp(body + 36, "data", 4) == 0, "data на месте");
    CHECK(b[40] == 0x20 && b[41] == 0x03, "размер data = 800 байт");
    CHECK(b[44] == 0xE8 && b[45] == 0x03, "первый сэмпл 1000 в little-endian");
}

static void test_verify_outcomes()
{
    std::printf("[TEST] 14. исходы проверки: REJECTED, 422, 5xx, нет transcript...\n");
    const std::vector<int16_t> pcm(200, 700);

    reset_all();
    http_mock_set_response(200, "{\"transcript\": \"Странно работает.\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_REJECTED, "посторонний текст -> REJECTED");
    CHECK(std::strcmp(kouki_wake_verify_last_transcript(), "Странно работает.") == 0, "текст сохранён для лога");

    reset_all();
    http_mock_set_response(422, "{\"error\":\"no speech\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_REJECTED, "HTTP 422 -> REJECTED");

    reset_all();
    http_mock_set_response(500, "{}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_UNAVAILABLE, "HTTP 500 -> UNAVAILABLE");

    reset_all();
    http_mock_set_response(200, "{\"text\": \"эй гин\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_UNAVAILABLE, "нет поля transcript -> UNAVAILABLE");

    reset_all();
    http_mock_set_response(200, "{\"transcript\": \"джин\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_ACCEPTED, "«джин» -> ACCEPTED");
}

static void test_verify_failures()
{
    std::printf("[TEST] 15. сбои проверки: open, запись, нет клиента, нет задачи...\n");
    const std::vector<int16_t> pcm(200, 700);

    reset_all();
    http_mock_fail_open(ESP_FAIL);
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_UNAVAILABLE, "open fail -> UNAVAILABLE");

    reset_all();
    http_mock_set_response(200, "{\"transcript\": \"эй гин\"}");
    http_mock_fail_write(0);
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_UNAVAILABLE, "сбой записи -> UNAVAILABLE");

    reset_all();
    http_mock_fail_next_init();
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_UNAVAILABLE, "нет клиента -> UNAVAILABLE");

    reset_all();
    wake_mock_fail_next_task_create();
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_ERR_NO_MEM,
          "нет задачи -> NO_MEM");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_UNAVAILABLE, "состояние UNAVAILABLE");
    CHECK(!wake_mock_run_task(), "запускать нечего");

    /* После ошибки запуск снова возможен (буфер и job освобождены). */
    http_mock_set_response(200, "{\"transcript\": \"эй гин\"}");
    CHECK(kouki_wake_verify_start(pcm.data(), static_cast<int>(pcm.size())) == ESP_OK, "повторный запуск -> ESP_OK");
    CHECK(wake_mock_run_task(), "задача прогнана");
    CHECK(kouki_wake_verify_state() == KOUKI_WAKE_VERIFY_ACCEPTED, "повторный запуск работает");
}

// ------------------------------------------------------------------ фасад ---

static void test_facade()
{
    std::printf("[TEST] 16. C-фасад проксирует классы 1:1...\n");
    reset_all();
    CHECK(kouki_wake_init() == kouki::wake().init(), "kouki_wake_init проксирует класс");
    CHECK(kouki_wake_is_ready() == kouki::wake().is_ready(), "kouki_wake_is_ready проксирует класс");
    CHECK(std::strcmp(kouki_wake_model(), kouki::wake().model()) == 0, "kouki_wake_model проксирует класс");
    CHECK(kouki_wake_verify_state() == kouki::wake_verify().state(), "состояние verify проксируется");
    CHECK(std::strcmp(kouki_wake_verify_last_transcript(), kouki::wake_verify().last_transcript()) == 0,
          "текст verify проксируется");
    CHECK(kouki_wake_verify_matches("эй гин") == kouki::WakeVerify::matches("эй гин"),
          "matches проксируется статическим методом");
}

// --------------------------------------------------------------- запуск -----

int main()
{
    std::printf("=== T121 П9: kouki::Wake + kouki::WakeVerify ===\n");

    test_init_disabled();
    test_init_and_reinit();
    test_init_no_mem();
    test_mute_after_init();
    test_mute_and_reset_facade();
    test_preroll_ring();
    test_energy_filter();
    test_fixture_wake_word();
    test_fixture_threshold();
    test_ready_follows_config();
    test_matches();
    test_verify_start_args();
    test_verify_request_shape();
    test_verify_outcomes();
    test_verify_failures();
    test_facade();

    std::printf("=== Wake: %d проверок PASS ===\n", g_checks);
    return 0;
}
