// Host-стенд T121 П6: класс kouki::Vad + его C-фасад.
//
// На хосте WebRTC VAD недоступен, поэтому работает та же ветка, что и в
// прошивке под #else: фреймы 30 мс (480 сэмплов) оцениваются по средней
// амплитуде (порог 300), а host_set_mock() позволяет задать ответ жёстко.
// Стенд проверяет именно поведение класса: нарезку на фреймы, FIFO остатка,
// streak, сброс и повторную инициализацию.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_vad.h"
#include "app_vad.hpp"

static int g_checks = 0;

#define CHECK(cond, msg) do {                                                 \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        std::exit(1);                                                         \
    }                                                                         \
} while (0)

namespace {

constexpr size_t kFrame = kouki::Vad::kFrameSamples;  // 480

/** Фрейм тишины. */
void fill_silence(int16_t *buf, size_t samples)
{
    std::memset(buf, 0, samples * sizeof(int16_t));
}

/** Фрейм «речи»: постоянная амплитуда выше порога 300. */
void fill_speech(int16_t *buf, size_t samples, int16_t amp = 1000)
{
    for (size_t i = 0; i < samples; i++) {
        buf[i] = amp;
    }
}

}  // namespace

// ------------------------------------------------------------------ тесты --

static void test_init_deinit()
{
    std::printf("[TEST] 1. init/deinit: идемпотентность и is_active...\n");

    kouki_vad_deinit();
    CHECK(!kouki_vad_is_active(), "после deinit модуль неактивен");

    CHECK(kouki_vad_init() == ESP_OK, "init -> ESP_OK");
    CHECK(kouki_vad_is_active(), "после init модуль активен");
    CHECK(kouki_vad_init() == ESP_OK, "повторный init -> ESP_OK");
    CHECK(kouki_vad_is_active(), "повторный init не сбрасывает активность");

    kouki_vad_deinit();
    CHECK(!kouki_vad_is_active(), "deinit снимает активность");
    kouki_vad_deinit();
    CHECK(!kouki_vad_is_active(), "повторный deinit безопасен");

    // init после deinit снова поднимает модуль.
    CHECK(kouki_vad_init() == ESP_OK, "init после deinit -> ESP_OK");
    CHECK(kouki_vad_is_active(), "модуль снова активен");
}

static void test_frame_splitting()
{
    std::printf("[TEST] 2. Нарезка на фреймы 30 мс и FIFO остатка...\n");

    kouki_vad_init();
    kouki_vad_reset();

    int16_t speech[kFrame];
    int16_t silence[kFrame];
    fill_speech(speech, kFrame);
    fill_silence(silence, kFrame);

    // Половина фрейма — речи ещё нет, streak не растёт.
    CHECK(!kouki_vad_process(speech, kFrame / 2), "полфрейма -> речи нет");
    CHECK(kouki_vad_get_speech_streak() == 0, "streak 0 на неполном фрейме");

    // Вторая половина замыкает фрейм — теперь речь.
    CHECK(kouki_vad_process(speech + kFrame / 2, kFrame / 2), "вторые полфрейма -> речь");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak 1 после первого фрейма");

    // Фрейм тишины обнуляет streak.
    CHECK(!kouki_vad_process(silence, kFrame), "фрейм тишины -> речи нет");
    CHECK(kouki_vad_get_speech_streak() == 0, "streak сброшен тишиной");

    // Три фрейма одним чанком.
    int16_t three[3 * kFrame];
    fill_speech(three, 3 * kFrame);
    CHECK(kouki_vad_process(three, 3 * kFrame), "чанк из 3 фреймов -> речь");
    CHECK(kouki_vad_get_speech_streak() == 3, "streak 3 за один вызов");
    CHECK(kouki_vad_is_speech_streak(3), "is_speech_streak(3) истинно");
    CHECK(!kouki_vad_is_speech_streak(4), "is_speech_streak(4) ложно");

    // Остаток между вызовами копится: 700 = фрейм + 220 в FIFO.
    kouki_vad_reset();
    int16_t chunk[700];
    fill_speech(chunk, 700);
    CHECK(kouki_vad_process(chunk, 700), "700 сэмплов -> один фрейм речи");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak 1, остаток в FIFO");
    int16_t tail[260];
    fill_speech(tail, 260);  // 220 + 260 = 480, второй фрейм
    CHECK(kouki_vad_process(tail, 260), "добивка остатка -> второй фрейм");
    CHECK(kouki_vad_get_speech_streak() == 2, "streak 2: остаток не потерян");
}

static void test_streak_semantics()
{
    std::printf("[TEST] 3. Смысл streak: серия подряд, возврат process...\n");

    kouki_vad_init();
    kouki_vad_reset();

    int16_t speech[kFrame];
    int16_t silence[kFrame];
    fill_speech(speech, kFrame);
    fill_silence(silence, kFrame);

    for (int i = 1; i <= 5; i++) {
        CHECK(kouki_vad_process(speech, kFrame), "фрейм речи возвращает true");
        CHECK(kouki_vad_get_speech_streak() == (uint32_t)i, "streak растёт по одному");
    }

    // Прерывание серии обнуляет счётчик и возвращает false на чистом фрейме.
    CHECK(!kouki_vad_process(silence, kFrame), "тишина после серии -> false");
    CHECK(kouki_vad_get_speech_streak() == 0, "тишина обнуляет серию");

    // Неполный фрейм после речи возвращает true не из-за фрейма, а из-за streak.
    CHECK(kouki_vad_process(speech, kFrame), "фрейм речи");
    CHECK(kouki_vad_process(silence, 100), "неполный фрейм тишины -> true из-за streak");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak не изменился неполным фреймом");

    // reset() обнуляет и streak, и FIFO: остаток 100 сэмплов тишины потерян.
    kouki_vad_reset();
    CHECK(kouki_vad_get_speech_streak() == 0, "reset обнуляет streak");
    CHECK(!kouki_vad_process(speech, kFrame - 100), "после reset FIFO пуст");
    CHECK(kouki_vad_get_speech_streak() == 0, "фрейм не собрался из остатка до reset");
}

static void test_energy_path()
{
    std::printf("[TEST] 4. Энергетический путь: порог 300, границы...\n");

    kouki_vad_init();
    kouki_vad_reset();
    kouki_vad_host_set_mock(false, false);

    int16_t below[kFrame];
    int16_t above[kFrame];
    int16_t edge[kFrame];
    fill_speech(below, kFrame, 299);
    fill_speech(above, kFrame, 300);
    fill_speech(edge, kFrame, 301);

    CHECK(!kouki_vad_process(below, kFrame), "амплитуда 299 < порога -> тишина");
    CHECK(kouki_vad_get_speech_streak() == 0, "streak 0 на 299");

    CHECK(kouki_vad_process(edge, kFrame), "амплитуда 301 > порога -> речь");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak 1 на 301");

    kouki_vad_reset();
    CHECK(kouki_vad_process(above, kFrame), "амплитуда ровно 300 -> речь (mean >= 300)");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak 1 на 300");

    // Отрицательные сэмплы считаются по модулю.
    kouki_vad_reset();
    int16_t negative[kFrame];
    fill_speech(negative, kFrame, -1000);
    CHECK(kouki_vad_process(negative, kFrame), "отрицательная амплитуда -> речь");
    CHECK(kouki_vad_get_speech_streak() == 1, "модуль амплитуды учтён");
}

static void test_mock_path()
{
    std::printf("[TEST] 5. Мок: принудительный ответ, вкл/выкл...\n");

    kouki_vad_init();
    kouki_vad_reset();

    int16_t silence[kFrame];
    int16_t speech[kFrame];
    fill_silence(silence, kFrame);
    fill_speech(speech, kFrame);

    kouki_vad_host_set_mock(true, true);
    CHECK(kouki_vad_process(silence, kFrame), "мок речь: тишина считается речью");
    CHECK(kouki_vad_get_speech_streak() == 1, "мок речь: streak растёт");
    CHECK(kouki_vad_process(NULL, 0) == false, "NULL всё равно даёт false");

    kouki_vad_host_set_mock(true, false);
    CHECK(!kouki_vad_process(speech, kFrame), "мок тишина: речь считается тишиной");
    CHECK(kouki_vad_get_speech_streak() == 0, "мок тишина: streak обнулён");

    // Мок выключен — снова работает энергетический путь.
    kouki_vad_host_set_mock(false, false);
    CHECK(kouki_vad_process(speech, kFrame), "без мока речь распознаётся");
    CHECK(!kouki_vad_process(silence, kFrame), "без мока тишина не распознаётся");
}

static void test_guards_and_autoreset()
{
    std::printf("[TEST] 6. Границы: NULL/0, автозапуск, повторный init...\n");

    // process() при неинициализированном модуле сам его поднимает.
    kouki_vad_deinit();
    CHECK(!kouki_vad_is_active(), "модуль выключен перед проверкой автозапуска");
    int16_t speech[kFrame];
    fill_speech(speech, kFrame);
    CHECK(kouki_vad_process(speech, kFrame), "process сам вызвал init и нашёл речь");
    CHECK(kouki_vad_is_active(), "модуль активен после автозапуска");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak 1 после автозапуска");

    // NULL/пустой чанк не трогает ни streak, ни FIFO.
    kouki_vad_reset();
    CHECK(!kouki_vad_process(NULL, kFrame), "NULL буфер -> false");
    CHECK(!kouki_vad_process(speech, 0), "0 сэмплов -> false");
    CHECK(kouki_vad_get_speech_streak() == 0, "streak не тронут");

    CHECK(!kouki_vad_process(NULL, 0), "NULL и 0 -> false");
    CHECK(kouki_vad_is_speech_streak(0), "is_speech_streak(0) истинно всегда");

    // init не сбрасывает накопленный streak (в C-версии было так же).
    kouki_vad_init();
    CHECK(kouki_vad_process(speech, kFrame), "фрейм речи");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak 1");
    CHECK(kouki_vad_init() == ESP_OK, "повторный init -> ESP_OK");
    CHECK(kouki_vad_get_speech_streak() == 1, "streak сохранён при повторном init");

    // deinit обнуляет состояние.
    kouki_vad_deinit();
    CHECK(kouki_vad_get_speech_streak() == 0, "deinit обнуляет streak");
    CHECK(!kouki_vad_is_speech_streak(1), "deinit обнуляет серию");
}

static void test_facade_and_instance()
{
    std::printf("[TEST] 7. Фасад и класс — один экземпляр...\n");

    CHECK(&kouki::vad() == &kouki::vad(), "vad() — один экземпляр");

    kouki_vad_deinit();
    CHECK(kouki::vad().init() == ESP_OK, "метод init() -> ESP_OK");
    CHECK(kouki::vad().is_active(), "метод is_active()");
    CHECK(kouki_vad_is_active(), "C-фасад видит состояние экземпляра");

    int16_t speech[kFrame];
    fill_speech(speech, kFrame);
    CHECK(kouki::vad().process(speech, kFrame), "метод process()");
    CHECK(kouki_vad_get_speech_streak() == 1, "фасад читает тот же streak");
    CHECK(kouki::vad().speech_streak() == 1, "метод speech_streak()");
    CHECK(kouki::vad().is_speech_streak(1), "метод is_speech_streak()");

    kouki::vad().reset();
    CHECK(kouki_vad_get_speech_streak() == 0, "метод reset() виден через фасад");
}

int main()
{
    std::printf("========================================\n");
    std::printf("      HOST CHECK: T121 Vad              \n");
    std::printf("========================================\n");

    test_init_deinit();
    test_frame_splitting();
    test_streak_semantics();
    test_energy_path();
    test_mock_path();
    test_guards_and_autoreset();
    test_facade_and_instance();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d проверок успешно пройдено!\n", g_checks);
    std::printf("========================================\n");
    return 0;
}
