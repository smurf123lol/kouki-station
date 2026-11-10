// Host-стенд T121 П10: класс kouki::Speech и его C-фасад.
//
// Стенд t116 (test_speech_host.c) проверяет сценарии озвучивания через
// C-фасад: HTTP-синтез, чанки, поколения, abort, блокировки кодека. Этот стенд
// дополняет его тем, что видно только на уровне класса и разбора:
//   * kouki::Speech::wav_parse на границах формата (невыровненные чанки,
//     data раньше fmt, size больше файла, сжатый формат, 3 канала, пустой
//     data) — то, что в C-модуле было внутренней статикой;
//   * kouki::Speech::stereo_expand на вырожденных входах;
//   * состояние класса до и после вызовов: busy()/is_playing()/
//     worker_active() тихи, reserve() монотонен, пустой текст и режим тишины
//     не трогают сеть, устаревшее поколение отбивается без HTTP;
//   * C-фасад и класс дают один и тот же результат (фасад — однострочник).
//
// Моки — support/speech (кодек, HTTP, heap_caps, время) и настоящий
// kouki::Config (config_mock), как в стенде t116.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.h"
#include "app_speech.h"
#include "app_speech.hpp"

extern int g_http_calls;
extern int g_play_calls;
extern int g_large_alloc_calls;
extern int g_wake_mute_calls;
extern int g_http_status;

extern "C" {
void fake_speech_reset(void);
void fake_http_set_samples(const int16_t *samples, size_t count_i16);
}

namespace {

int g_checks = 0;

#define CHECK(cond, msg) do {                                                 \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        std::exit(1);                                                         \
    }                                                                         \
} while (0)

/** Собирает минимальный WAV: fmt 16 PCM, data; возвращает длину файла. */
size_t make_wav(uint8_t *wav, const int16_t *samples, size_t count, uint16_t channels = 1,
                uint32_t rate = 16000, uint16_t bits = 16, uint16_t format = 1)
{
    const size_t data_bytes = count * sizeof(int16_t);
    memset(wav, 0, 44 + data_bytes);
    memcpy(wav + 0, "RIFF", 4);
    const uint32_t riff = (uint32_t)(36 + data_bytes);
    wav[4] = (uint8_t)riff;
    wav[5] = (uint8_t)(riff >> 8);
    wav[6] = (uint8_t)(riff >> 16);
    wav[7] = (uint8_t)(riff >> 24);
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    wav[16] = 16;
    wav[20] = (uint8_t)format;
    wav[21] = (uint8_t)(format >> 8);
    wav[22] = (uint8_t)channels;
    wav[24] = (uint8_t)rate;
    wav[25] = (uint8_t)(rate >> 8);
    wav[26] = (uint8_t)(rate >> 16);
    wav[27] = (uint8_t)(rate >> 24);
    const uint32_t byte_rate = rate * channels * (bits / 8);
    wav[28] = (uint8_t)byte_rate;
    wav[29] = (uint8_t)(byte_rate >> 8);
    wav[30] = (uint8_t)(byte_rate >> 16);
    wav[32] = (uint8_t)(channels * (bits / 8));
    wav[34] = (uint8_t)bits;
    memcpy(wav + 36, "data", 4);
    wav[40] = (uint8_t)data_bytes;
    wav[41] = (uint8_t)(data_bytes >> 8);
    wav[42] = (uint8_t)(data_bytes >> 16);
    wav[43] = (uint8_t)(data_bytes >> 24);
    memcpy(wav + 44, samples, data_bytes);
    return 44 + data_bytes;
}

void reset_all()
{
    fake_speech_reset();
    kouki_config_set_silent(false);
}

// ------------------------------------------------------------- wav_parse ---

void test_wav_parse_boundaries()
{
    const int16_t samples[4] = {1, 2, 3, 4};
    uint8_t wav[256];
    kouki_speech_wav_t info;

    size_t len = make_wav(wav, samples, 4);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_OK, "16 кГц моно PCM16 разобран");
    CHECK(info.data_offset == 44 && info.data_bytes == 8 && info.channels == 1 &&
              info.sample_rate == 16000 && info.bits_per_sample == 16,
          "поля разобранного WAV как в заголовке");

    // Стерео принимается, но каналы сообщаются наружу: разворот делает play.
    len = make_wav(wav, samples, 4, 2);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_OK && info.channels == 2,
          "стерео PCM16 разобран, channels=2");

    // 3 канала — не поддерживаем.
    len = make_wav(wav, samples, 4, 3);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_NOT_SUPPORTED,
          "3 канала -> ESP_ERR_NOT_SUPPORTED");

    // Сжатый формат (mp3/aac станция не декодирует).
    len = make_wav(wav, samples, 4, 1, 16000, 16, 0x55);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_NOT_SUPPORTED,
          "сжатый формат -> ESP_ERR_NOT_SUPPORTED");

    // 8 бит на сэмпл.
    len = make_wav(wav, samples, 4, 1, 16000, 8);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_NOT_SUPPORTED,
          "8 бит на сэмпл -> ESP_ERR_NOT_SUPPORTED");

    // Не RIFF/WAVE.
    len = make_wav(wav, samples, 4);
    wav[0] = 'X';
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_INVALID_RESPONSE,
          "не RIFF -> ESP_ERR_INVALID_RESPONSE");
    wav[0] = 'R';
    wav[8] = 'X';
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_INVALID_RESPONSE,
          "не WAVE -> ESP_ERR_INVALID_RESPONSE");
    wav[8] = 'W';

    // Короче заголовка и пустые аргументы.
    CHECK(kouki::Speech::wav_parse(wav, 11, &info) == ESP_ERR_INVALID_RESPONSE,
          "11 байт -> ESP_ERR_INVALID_RESPONSE");
    CHECK(kouki::Speech::wav_parse(nullptr, len, &info) == ESP_ERR_INVALID_ARG,
          "wav==NULL -> ESP_ERR_INVALID_ARG");
    CHECK(kouki::Speech::wav_parse(wav, len, nullptr) == ESP_ERR_INVALID_ARG,
          "out==NULL -> ESP_ERR_INVALID_ARG");

    // data раньше fmt: разбор не имеет права принимать такой файл.
    len = make_wav(wav, samples, 4);
    memcpy(wav + 12, "data", 4);
    memcpy(wav + 36, "fmt ", 4);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_INVALID_RESPONSE,
          "data раньше fmt -> ESP_ERR_INVALID_RESPONSE");

    // size больше, чем осталось в файле: не прыгаем за границу буфера.
    len = make_wav(wav, samples, 4);
    wav[40] = 0xFF;
    wav[41] = 0xFF;
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_INVALID_RESPONSE,
          "size больше файла -> ESP_ERR_INVALID_RESPONSE");

    // Пустой data.
    len = make_wav(wav, samples, 0);
    CHECK(kouki::Speech::wav_parse(wav, len, &info) == ESP_ERR_INVALID_RESPONSE,
          "пустой data -> ESP_ERR_INVALID_RESPONSE");

    // Невыровненный по чётной границе чанк перед data: разбор обязан
    // перепрыгнуть паддинг и всё равно найти fmt/data.
    {
        uint8_t extra[300];
        memset(extra, 0, sizeof(extra));
        const int16_t two[2] = {7, 8};
        const size_t base = make_wav(wav, two, 2);
        // fmt-чанк (12..35) остаётся, перед data вставляем чанк "LIST" с
        // нечётным размером 3 и паддингом 1 байт.
        const uint8_t chunk[8 + 3 + 1] = {'L', 'I', 'S', 'T', 3, 0, 0, 0, 'a', 'b', 'c', 0};
        memcpy(extra, wav, 36);
        memcpy(extra + 36, chunk, sizeof(chunk));
        memcpy(extra + 36 + sizeof(chunk), wav + 36, base - 36);
        const size_t len2 = 36 + sizeof(chunk) + (base - 36);
        // Смещения в заголовке data пересчитались вместе с вставкой, поэтому
        // проверяем только факт успешного разбора и границы сэмплов.
        const esp_err_t err = kouki::Speech::wav_parse(extra, len2, &info);
        CHECK(err == ESP_OK, "невыровненный чанк перед data пропущен");
        CHECK(info.data_bytes == 4 && info.data_offset == 36 + sizeof(chunk) + 8,
              "data_offset учитывает паддинг невыровненного чанка");
    }
}

// -------------------------------------------------------- stereo_expand ---

void test_stereo_expand_edges()
{
    CHECK(kouki::Speech::stereo_expand(nullptr, 0) == ESP_ERR_INVALID_ARG,
          "stereo_expand(NULL) -> ESP_ERR_INVALID_ARG");

    int16_t one[2] = {42, 0};
    CHECK(kouki::Speech::stereo_expand(one, 1) == ESP_OK && one[0] == 42 && one[1] == 42,
          "один сэмпл разворачивается в пару L=R");

    int16_t none[1] = {0};
    CHECK(kouki::Speech::stereo_expand(none, 0) == ESP_OK,
          "нулевое число фреймов — не ошибка");

    // Разворот идёт с конца: исходные значения не затираются до чтения.
    int16_t four[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    CHECK(kouki::Speech::stereo_expand(four, 4) == ESP_OK && four[0] == 1 && four[1] == 1 &&
              four[2] == 2 && four[3] == 2 && four[4] == 3 && four[5] == 3 && four[6] == 4 &&
              four[7] == 4,
          "разворот с конца не затирает ещё не прочитанные сэмплы");
}

// --------------------------------------------------- состояние класса ---

void test_class_state_and_guards()
{
    reset_all();

    // До первого вызова класс тих: никаких поколений и воркеров.
    CHECK(!kouki::speech().busy(), "busy() ложно до первого вызова");
    CHECK(!kouki::speech().is_playing(), "is_playing() ложно до первого вызова");
    CHECK(!kouki::speech().worker_active(), "worker_active() ложно до первого вызова");

    // reserve() монотонен и каждое поколение новое.
    const uint32_t g1 = kouki::speech().reserve();
    const uint32_t g2 = kouki::speech().reserve();
    CHECK(g2 == g1 + 1, "reserve() выдаёт следующее поколение");

    // Пустой текст — не ошибка и не ходит в сеть.
    CHECK(kouki::speech().play("") == ESP_OK, "play(\"\") -> ESP_OK");
    CHECK(kouki::speech().play(nullptr) == ESP_OK, "play(NULL) -> ESP_OK");
    CHECK(g_http_calls == 0 && g_play_calls == 0, "пустой текст не трогает сеть и кодек");

    // Режим тишины: успех без сети и без звука, как в C-модуле.
    kouki_config_set_silent(true);
    CHECK(kouki::speech().play("Тихий ответ") == ESP_OK, "play() в тишине -> ESP_OK");
    CHECK(g_http_calls == 0 && g_play_calls == 0 && g_wake_mute_calls == 0,
          "режим тишины не ходит в сеть, не играет и не мьютит wake");
    kouki_config_set_silent(false);

    // Устаревшее поколение отбивается до HTTP: зарезервировали два, играем
    // первое — оно уже не актуально.
    reset_all();
    const int16_t samples[2] = {5, 6};
    fake_http_set_samples(samples, 2);
    const uint32_t stale = kouki::speech().reserve();
    (void)kouki::speech().reserve();
    CHECK(kouki::speech().play_gen("Устаревшее", stale) == ESP_ERR_INVALID_STATE,
          "устаревшее поколение -> ESP_ERR_INVALID_STATE");
    CHECK(g_http_calls == 0 && g_play_calls == 0 && !kouki::speech().busy(),
          "отбитое поколение не ходит в сеть и не оставляет busy");

    // Свежее поколение проходит полностью и после него класс снова тих.
    reset_all();
    fake_http_set_samples(samples, 2);
    const uint32_t fresh = kouki::speech().reserve();
    CHECK(kouki::speech().play_gen("Актуальное", fresh) == ESP_OK,
          "актуальное поколение озвучивается");
    CHECK(g_http_calls == 1 && g_play_calls == 2, "HTTP один раз, звук — чанк + финальная пауза");
    CHECK(!kouki::speech().busy() && !kouki::speech().is_playing() &&
              !kouki::speech().worker_active(),
          "после штатного завершения класс снова тих");

    // abort() без активного озвучивания безопасен и не оставляет состояния.
    kouki::speech().abort();
    CHECK(!kouki::speech().busy() && !kouki::speech().worker_active(),
          "abort() в простое ничего не ломает");
}

// --------------------------------------------------- фасад против класса ---

void test_facade_matches_class()
{
    const int16_t samples[3] = {1000, -1000, 2000};
    uint8_t a[256];
    uint8_t b[256];
    const size_t len_a = make_wav(a, samples, 3);
    const size_t len_b = make_wav(b, samples, 3);

    kouki_speech_wav_t via_class;
    kouki_speech_wav_t via_facade;
    CHECK(kouki::Speech::wav_parse(a, len_a, &via_class) == ESP_OK, "разбор через класс");
    CHECK(kouki_speech_wav_parse(b, len_b, &via_facade) == ESP_OK, "разбор через фасад");
    CHECK(via_class.data_offset == via_facade.data_offset &&
              via_class.data_bytes == via_facade.data_bytes &&
              via_class.channels == via_facade.channels &&
              via_class.sample_rate == via_facade.sample_rate &&
              via_class.bits_per_sample == via_facade.bits_per_sample,
          "фасад kouki_speech_wav_parse отдаёт то же, что static-метод класса");

    int16_t x[4] = {1, 2, 3, 4};
    int16_t y[4] = {1, 2, 3, 4};
    CHECK(kouki::Speech::stereo_expand(x, 2) == ESP_OK, "разворот через класс");
    CHECK(kouki_speech_stereo_expand(y, 2) == ESP_OK, "разворот через фасад");
    CHECK(memcmp(x, y, sizeof(x)) == 0, "фасад kouki_speech_stereo_expand = метод класса");

    // Фасадные предикаты читают тот же экземпляр, что и класс.
    reset_all();
    CHECK(kouki_speech_busy() == kouki::speech().busy(), "kouki_speech_busy() = busy()");
    CHECK(kouki_speech_is_playing() == kouki::speech().is_playing(),
          "kouki_speech_is_playing() = is_playing()");
    CHECK(kouki_speech_worker_active() == kouki::speech().worker_active(),
          "kouki_speech_worker_active() = worker_active()");

    // Резерв через фасад двигает то же поколение класса.
    const uint32_t via_facade_gen = kouki_speech_reserve();
    const uint32_t via_class_gen = kouki::speech().reserve();
    CHECK(via_class_gen == via_facade_gen + 1, "фасад и класс делят счётчик поколений");
}

}  // namespace

int main()
{
    std::printf("=== T121 П10: kouki::Speech + C-фасад ===\n");
    reset_all();
    test_wav_parse_boundaries();
    test_stereo_expand_edges();
    test_class_state_and_guards();
    test_facade_matches_class();
    std::printf("=== Speech: %d проверок PASS ===\n", g_checks);
    return 0;
}
