// Host-стенд T121 П5: классы kouki::AudioOut и kouki::Tone + их C-фасады.
//
// Кодек заменён моком (support/audio/audio_mock.c): esp_audio_play запоминает
// размер и первые сэмплы блока, поэтому стенд проверяет настоящую очередь,
// настоящий worker и настоящий порядок воспроизведения — без I2S и ES8311.
//
// Файл собирается дважды: с CONFIG_KOUKI_SIGNAL_VOLUME=50 (обычный) и =0
// (каталог quiet/) — во втором случае Tone обязан молчать и не трогать кодек.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app_audio_out.h"
#include "app_audio_out.hpp"
#include "app_tone.h"
#include "app_tone.hpp"

#ifndef TONE_EXPECT_VOLUME_0
#define TONE_EXPECT_VOLUME_0 0
#endif

// ------------------------------------------------------------- мок кодека --
#include "audio_mock.h"

// Заглушка wake-word: Tone после сигнала подавляет детектор на 250 мс.
static int g_wake_mute_calls = 0;
static int g_wake_mute_last_ms = 0;
extern "C" void kouki_wake_mute(int duration_ms)
{
    g_wake_mute_calls++;
    g_wake_mute_last_ms = duration_ms;
}

static int g_checks = 0;

#define CHECK(cond, msg) do {                                                 \
    g_checks++;                                                               \
    if (!(cond)) {                                                            \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);  \
        std::exit(1);                                                         \
    }                                                                         \
} while (0)

// ------------------------------------------------------------------ тесты --

static void test_init_and_lock()
{
    std::printf("[TEST] 1. init: идемпотентность, leaf-lock, один экземпляр...\n");

    CHECK(kouki_audio_out_init() == ESP_OK, "init -> ESP_OK");
    CHECK(kouki_audio_out_init() == ESP_OK, "повторный init -> ESP_OK (идемпотентно)");

    // lock/unlock сбалансированы и допускают вложение (мьютекс рекурсивный
    // по контракту вызывающего: в C-модуле это был xSemaphoreTake).
    kouki_audio_out_lock();
    kouki_audio_out_unlock();
    CHECK(true, "lock/unlock не падают и не зависают");

    CHECK(&kouki::audio_out() == &kouki::audio_out(), "audio_out() — один экземпляр");
    CHECK(kouki::audio_out().init() == ESP_OK, "метод init() -> ESP_OK");
}

static void test_queue_order()
{
    std::printf("[TEST] 2. Очередь: порядок воспроизведения и дробление на чанки...\n");

    audio_mock_reset();

    static int16_t a[4] = {111, 222, 333, 444};
    static int16_t b[2] = {555, 666};
    static int16_t c[2] = {777, 888};

    CHECK(kouki_audio_out_enqueue_pcm(a, sizeof(a), false) == ESP_OK, "enqueue A");
    CHECK(kouki_audio_out_enqueue_pcm(b, sizeof(b), false) == ESP_OK, "enqueue B");
    CHECK(kouki_audio_out_enqueue_pcm(c, sizeof(c), false) == ESP_OK, "enqueue C");

    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "wait_idle по всем трём");

    const int calls = audio_mock_play_calls();
    CHECK(calls == 3, "три блока -> три вызова кодека (по одному чанку на элемент)");
    CHECK(audio_mock_call(0)->first[0] == 111, "первым сыгран A");
    CHECK(audio_mock_call(1)->first[0] == 555, "вторым сыгран B");
    CHECK(audio_mock_call(2)->first[0] == 777, "третьим сыгран C");
    CHECK(audio_mock_call(0)->bytes == (int)sizeof(a), "A отдан целиком, байт 8");
    CHECK(!audio_mock_call(0)->silence, "A не тишина");

    // Большой буфер дробится на чанки по 3200 байт (~25 мс).
    audio_mock_reset();
    const size_t big_bytes = 3200 * 2 + 200;
    int16_t *big = (int16_t *)calloc(1, big_bytes);
    CHECK(big != NULL, "буфер большого элемента выделен");
    for (size_t i = 0; i < big_bytes / sizeof(int16_t); i++) {
        big[i] = (int16_t)(i % 1000);
    }
    CHECK(kouki_audio_out_enqueue_pcm(big, big_bytes, true) == ESP_OK, "enqueue большой");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "wait_idle большого");
    CHECK(audio_mock_play_calls() == 3, "big: 3200 + 3200 + 200 -> три чанка");
    CHECK(audio_mock_call(0)->bytes == 3200, "первый чанк 3200 байт");
    CHECK(audio_mock_call(1)->bytes == 3200, "второй чанк 3200 байт");
    CHECK(audio_mock_call(2)->bytes == 200, "хвостовой чанк 200 байт");
    CHECK(audio_mock_total_bytes() == (int)big_bytes, "суммарно отдано ровно столько же");
}

static void test_silence()
{
    std::printf("[TEST] 3. Тишина в очереди: шаги по 10 мс, нули...\n");

    audio_mock_reset();

    CHECK(kouki_audio_out_enqueue_silence(0) == ESP_OK, "silence(0) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 0, "silence(0) ничего не играет");

    CHECK(kouki_audio_out_enqueue_silence(25) == ESP_OK, "silence(25) в очередь");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "wait_idle после тишины");
    CHECK(audio_mock_play_calls() == 3, "25 мс -> 10 + 10 + 5 мс, три блока");
    CHECK(audio_mock_call(0)->bytes == 640, "10 мс * 64 байта");
    CHECK(audio_mock_call(1)->bytes == 640, "10 мс * 64 байта");
    CHECK(audio_mock_call(2)->bytes == 320, "5 мс * 64 байта");
    CHECK(audio_mock_call(0)->silence, "первый блок — нули");
    CHECK(audio_mock_call(2)->silence, "последний блок — нули");
    CHECK(audio_mock_total_bytes() == 25 * 64, "ровно 25 мс тишины");
}

static void test_abort()
{
    std::printf("[TEST] 4. abort: очередь сброшена, DMA продута тишиной...\n");

    audio_mock_reset();
    audio_mock_set_delay_us(50 * 1000); /* кодек «играет» 50 мс на блок */

    static int16_t x[3200];
    x[0] = 42;
    CHECK(kouki_audio_out_enqueue_pcm(x, sizeof(x), false) == ESP_OK, "enqueue X");
    CHECK(kouki_audio_out_is_busy(), "is_busy сразу после постановки");

    kouki_audio_out_abort();

    CHECK(!kouki_audio_out_is_busy(), "после abort очередь пуста и кодек молчит");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "wait_idle после abort -> ESP_OK");

    // abort обязан продуть DMA последним блоком тишины (640 байт).
    const int calls = audio_mock_play_calls();
    CHECK(calls >= 1, "abort сыграл хотя бы один блок");
    CHECK(audio_mock_call(calls - 1)->bytes == 640, "последний блок abort — 640 байт тишины");
    CHECK(audio_mock_call(calls - 1)->silence, "последний блок abort — нули");
    audio_mock_set_delay_us(0);
}

static void test_busy_idle()
{
    std::printf("[TEST] 5. is_busy/wait_idle: до и после опустошения...\n");

    audio_mock_reset();
    audio_mock_set_delay_us(100 * 1000);

    static int16_t y[3200];
    y[0] = 7;
    CHECK(!kouki_audio_out_is_busy(), "пустая очередь — не занят");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "wait_idle на пустой -> ESP_OK");

    CHECK(kouki_audio_out_enqueue_pcm(y, sizeof(y), false) == ESP_OK, "enqueue Y");
    CHECK(kouki_audio_out_is_busy(), "после постановки — занят");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "wait_idle дождался");
    CHECK(!kouki_audio_out_is_busy(), "после wait_idle — свободен");
    audio_mock_set_delay_us(0);
}

static void test_direct_play()
{
    std::printf("[TEST] 6. Прямое воспроизведение, ошибки кодека, пустые аргументы...\n");

    audio_mock_reset();
    static int16_t s[4] = {1, 2, 3, 4};

    CHECK(kouki_audio_play(NULL, 8, portMAX_DELAY) == ESP_OK, "play(NULL) -> ESP_OK");
    CHECK(kouki_audio_play(s, 0, portMAX_DELAY) == ESP_OK, "play(bytes=0) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 0, "пустые аргументы не доходят до кодека");

    CHECK(kouki_audio_play(s, sizeof(s), portMAX_DELAY) == ESP_OK, "play(4 сэмпла) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 1, "один вызов кодека");
    CHECK(audio_mock_call(0)->bytes == (int)sizeof(s), "переданные байты не изменены");

    // Ошибка кодека пробрасывается наружу как есть.
    audio_mock_reset();
    audio_mock_fail_at(1, ESP_FAIL);
    CHECK(kouki_audio_play(s, sizeof(s), portMAX_DELAY) == ESP_FAIL, "ошибка кодека -> ESP_FAIL");
    audio_mock_fail_at(0, ESP_OK);

    audio_mock_reset();
    CHECK(kouki_audio_play_silence(0, portMAX_DELAY) == ESP_OK, "play_silence(0) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 0, "play_silence(0) не играет");
    CHECK(kouki_audio_play_silence(20, portMAX_DELAY) == ESP_OK, "play_silence(20) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 2, "20 мс -> два блока по 10 мс");

    // play_with_tail: данные + хвост; без данных — только тишина.
    audio_mock_reset();
    CHECK(kouki_audio_play_with_tail(s, sizeof(s), 20, portMAX_DELAY) == ESP_OK,
          "play_with_tail -> ESP_OK");
    CHECK(audio_mock_play_calls() == 3, "данные + два блока хвоста");
    CHECK(!audio_mock_call(0)->silence, "сначала данные");
    CHECK(audio_mock_call(1)->silence, "потом тишина");
    audio_mock_reset();
    CHECK(kouki_audio_play_with_tail(NULL, 0, 10, portMAX_DELAY) == ESP_OK,
          "play_with_tail без данных -> ESP_OK");
    CHECK(audio_mock_play_calls() == 1, "только хвост тишины");
}

static void test_enqueue_guards()
{
    std::printf("[TEST] 7. Границы: NULL/ноль/переполнение, освобождение памяти...\n");

    audio_mock_reset();

    // NULL с transfer_ownership не должен падать.
    CHECK(kouki_audio_out_enqueue_pcm(NULL, 0, true) == ESP_OK, "enqueue(NULL, 0, true) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 0, "NULL не играет");

    // Нулевая длина с владением: память освобождается, ничего не играет.
    int16_t *zero = (int16_t *)malloc(8);
    CHECK(zero != NULL, "буфер выделен");
    CHECK(kouki_audio_out_enqueue_pcm(zero, 0, true) == ESP_OK, "enqueue(bytes=0, own) -> ESP_OK");
    CHECK(audio_mock_play_calls() == 0, "нулевая длина не играет");

    // Переполнение: держим leaf-lock сами — worker вынет один элемент и
    // встанет на кодеке, очередь заполнится до kQueueLen, следующий элемент
    // обязан получить ESP_ERR_TIMEOUT (как в C-модуле: xQueueSend + 1000 мс).
    static int16_t items[kouki::AudioOut::kQueueLen + 4][3200];
    std::memset(items, 1, sizeof(items));

    audio_mock_set_delay_us(200 * 1000);
    kouki_audio_out_lock();

    int ok = 0;
    esp_err_t last = ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < kouki::AudioOut::kQueueLen + 4; i++) {
        last = kouki_audio_out_enqueue_pcm(items[i], sizeof(items[i]), false);
        if (last == ESP_OK) {
            ok++;
        }
    }
    kouki_audio_out_unlock();

    CHECK(ok >= 1, "часть элементов принята в очередь");
    CHECK(ok <= (int)(kouki::AudioOut::kQueueLen + 4), "принято не больше, чем положили");
    CHECK(last == ESP_ERR_TIMEOUT, "переполнение -> ESP_ERR_TIMEOUT");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "очередь разошлась");
    audio_mock_set_delay_us(0);
}

// ------------------------------------------------------------------- Tone --

static void test_tone_pattern()
{
    std::printf("[TEST] 8. Tone: синтез, стерео, хвост тишины, mute детектора...\n");

    audio_mock_reset();
    g_wake_mute_calls = 0;

#if TONE_EXPECT_VOLUME_0
    // Громкость 0: сигнал пропускается целиком, кодек и детектор не трогаем.
    kouki_tone_play(KOUKI_TONE_START);
    CHECK(audio_mock_play_calls() == 0, "volume=0: кодек не задействован");
    CHECK(g_wake_mute_calls == 0, "volume=0: детектор не подавляется");
    CHECK(true, "Tone молчит при CONFIG_KOUKI_SIGNAL_VOLUME=0");
#else
    // START: один тон 100 мс -> 1600 кадров, стерео = 6400 байт, плюс 80 мс тишины.
    kouki_tone_play(KOUKI_TONE_START);
    CHECK(audio_mock_play_calls() == 1 + 8, "START: данные + 80 мс хвоста (8 блоков)");
    CHECK(audio_mock_call(0)->bytes == 6400, "START: 1600 кадров * 2 канала * 2 байта");
    CHECK(!audio_mock_call(0)->silence, "START: звук, не тишина");
    CHECK(audio_mock_call(0)->first[0] == 0, "fade-in: первый сэмпл ровно ноль");
    CHECK(audio_mock_call(0)->first[1] == 0, "правый канал тоже с нуля");
    CHECK(audio_mock_call(1)->silence, "после сигнала идёт продувка тишиной");
    CHECK(g_wake_mute_calls == 1, "детектор подавлен один раз");
    CHECK(g_wake_mute_last_ms == 250, "подавление на 250 мс");

    // WAKE: три шага (50 + 40 паузы + 50 мс) -> 140 мс, один блок + хвост.
    audio_mock_reset();
    kouki_tone_play(KOUKI_TONE_WAKE);
    CHECK(audio_mock_play_calls() == 1 + 8, "WAKE: один блок данных + хвост");
    CHECK(audio_mock_call(0)->bytes == 140 * 16 * 4, "WAKE: 140 мс стерео");
    CHECK(!audio_mock_call(0)->silence, "WAKE: звук");

    // ERROR: 150 мс.
    audio_mock_reset();
    kouki_tone_play(KOUKI_TONE_ERROR);
    CHECK(audio_mock_call(0)->bytes == 150 * 16 * 4, "ERROR: 150 мс стерео");

    // STOP_SENDING: 120 мс.
    audio_mock_reset();
    kouki_tone_play(KOUKI_TONE_STOP_SENDING);
    CHECK(audio_mock_call(0)->bytes == 120 * 16 * 4, "STOP_SENDING: 120 мс стерео");

    // Неизвестный сигнал: только лог, кодек не трогаем.
    audio_mock_reset();
    kouki_tone_play((kouki_tone_t)99);
    CHECK(audio_mock_play_calls() == 0, "неизвестный сигнал ничего не играет");

    // Ошибка кодека не роняет вызов: Tone логирует и всё равно подавляет детектор.
    audio_mock_reset();
    audio_mock_fail_at(1, ESP_FAIL);
    g_wake_mute_calls = 0;
    kouki_tone_play(KOUKI_TONE_START);
    CHECK(g_wake_mute_calls == 1, "после ошибки кодека детектор всё равно подавлен");
    audio_mock_fail_at(0, ESP_OK);
#endif
}

static void test_facade()
{
    std::printf("[TEST] 9. Фасады и классы — одни экземпляры...\n");

    CHECK(&kouki::tone() == &kouki::tone(), "tone() — один экземпляр");

    audio_mock_reset();
    static int16_t z[2] = {9, 9};
    CHECK(kouki::audio_out().enqueue_pcm(z, sizeof(z), false) == ESP_OK, "метод enqueue_pcm");
    CHECK(kouki::audio_out().wait_idle(portMAX_DELAY) == ESP_OK, "метод wait_idle");
    CHECK(audio_mock_play_calls() == 1, "метод идёт в тот же worker");
}

int main()
{
    std::printf("========================================\n");
    std::printf("   HOST CHECK: T121 AudioOut + Tone     \n");
    std::printf("========================================\n");

    test_init_and_lock();
    test_queue_order();
    test_silence();
    test_abort();
    test_busy_idle();
    test_direct_play();
    test_enqueue_guards();
    test_tone_pattern();
    test_facade();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d проверок успешно пройдено!\n", g_checks);
    std::printf("========================================\n");
    return 0;
}
