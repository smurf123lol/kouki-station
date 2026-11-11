// Host-стенд T121 П5: классы kouki::AudioOut и kouki::Tone + их C-фасады.
//
// Кодек заменён моком (support/audio/audio_mock.c): esp_audio_play запоминает
// размер и первые сэмплы блока, поэтому стенд проверяет настоящую очередь,
// настоящий worker и настоящий порядок воспроизведения — без I2S и ES8311.
//
// Файл собирается дважды: с CONFIG_KOUKI_SIGNAL_VOLUME=50 (обычный) и =0
// (каталог quiet/) — во втором случае Tone обязан молчать и не трогать кодек.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

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

    // lock/unlock — это leaf-lock вокруг TX-кодека, а не пустая пара вызовов.
    // Пункт 11 ревью П5: раньше здесь стоял CHECK(true, "не падают"), который
    // не мог упасть никогда. Проверяем наблюдаемый контракт: пока замок
    // взят вызывающим, worker не отдаёт блок кодеку, а после unlock — отдаёт.
    audio_mock_reset();
    static int16_t probe[3200];
    probe[0] = 3;

    kouki_audio_out_lock();
    CHECK(kouki_audio_out_enqueue_pcm(probe, sizeof(probe), false) == ESP_OK,
          "enqueue под взятым замком");
    // worker успел бы отдать блок кодеку, если бы замок не держал
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(audio_mock_play_calls() == 0,
          "пока замок взят, кодек блок не получает (leaf-lock работает)");

    kouki_audio_out_unlock();
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK,
          "после unlock очередь разошлась");
    // 3200 сэмплов = 6400 байт -> worker дробит блок на два чанка по 3200.
    CHECK(audio_mock_play_calls() == 2, "после unlock блок доигран (два чанка)");

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

/**
 * Сколько байт сейчас занято в куче (для проверки утечки во владеющем enqueue).
 *
 * uordblks считает занятые пользователем блоки; после free он возвращается к
 * прежнему значению, поэтому годится как дельта «до/после». На платформах без
 * mallinfo2 дельта всегда 0, и проверка утечки превращается в no-op — тест
 * остаётся корректным, но слабее (см. README стенда).
 */
static long heap_in_use()
{
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
    struct mallinfo2 mi = mallinfo2();
    return (long)mi.uordblks;
#else
    return 0;
#endif
}

/**
 * Пункт 9 ревью П5 (ВАЖНЫЙ): enqueue_pcm с владением при ESP_ERR_TIMEOUT
 * обязан освободить буфер.
 *
 * Мутант a19 убирает free(data) в ветке переполнения — вызывающий уже отдал
 * владение, поэтому буфер не освобождает никто: утечка heap_delta 54672 ->
 * 77184 (+22512 = 7 x 3200). Проверка идёт по дельте занятой кучи, а не по
 * счётчику free в моке: free вызывает сам модуль, а не заглушка.
 */
static void test_enqueue_ownership_on_timeout()
{
    std::printf("[TEST] 7б. Переполнение с владением: буфер освобождён...\n");

    audio_mock_reset();
    const size_t elem = 3200;
    // Разогрев: первый malloc в процессе тянет за собой арену, её рост дал бы
    // ложную дельту. Выделяем и сразу освобождаем буфер того же размера.
    void *warm = malloc(elem);
    free(warm);

    audio_mock_set_delay_us(200 * 1000);
    kouki_audio_out_lock();

    // Набиваем очередь без владения: эти буферы статический массив, их никто
    // не освобождает, и на дельту они не влияют.
    static int16_t filler[kouki::AudioOut::kQueueLen + 4][3200];
    std::memset(filler, 1, sizeof(filler));
    int queued = 0;
    for (size_t i = 0; i < kouki::AudioOut::kQueueLen + 4; i++) {
        if (kouki_audio_out_enqueue_pcm(filler[i], sizeof(filler[i]), false) != ESP_OK) {
            break;
        }
        queued++;
    }
    CHECK(queued > 0, "очередь заполнена элементами без владения");

    const long before = heap_in_use();
    int timed_out = 0;
    long allocated_total = 0;
    // Пока очередь полна, каждый владеющий элемент обязан либо встать в
    // очередь, либо вернуть ESP_ERR_TIMEOUT и освободить буфер.
    for (int i = 0; i < 7; i++) {
        int16_t *owned = (int16_t *)malloc(elem);
        CHECK(owned != NULL, "буфер под владеющий элемент выделен");
        allocated_total += (long)elem;
        if (kouki_audio_out_enqueue_pcm(owned, elem, true) == ESP_ERR_TIMEOUT) {
            timed_out++;
        }
    }
    const long after = heap_in_use();
    kouki_audio_out_unlock();
    audio_mock_set_delay_us(0);
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "очередь разошлась");

    CHECK(timed_out > 0, "часть владеющих элементов получила ESP_ERR_TIMEOUT");
    // Принятые в очередь буферы ещё висят в очереди (worker заблокирован на
    // кодеке) — на них куча законно выросла. Отказные освобождает сам
    // enqueue_pcm, и именно их мутант a19 теряет.
    const long allowed = allocated_total - (long)timed_out * (long)elem;
    CHECK(after - before <= allowed,
          "отказные буферы освобождены: прирост кучи не больше принятых в очередь");
}

/**
 * Пункт 8 ревью П5 (мутант a8): guard «NULL или нулевая длина» в enqueue_pcm.
 *
 * Guard стоит ДО ready()/init() и ДО постановки в очередь. Без него:
 * (а) буфер нулевой длины с владением никто не освободит — worker играет
 * только `bytes > 0`, поэтому free в его ветке не выполнится;
 * (б) в очередь попадёт пустой Item (data = NULL, bytes = 0), и is_busy()
 * на мгновение станет true — «занят, хотя играть нечего».
 *
 * Поведение 1:1 с master (db4bd1b:171-178): и NULL/0, и нулевая длина
 * возвращают ESP_OK, а не ESP_ERR_INVALID_ARG — это не ошибка вызывающего,
 * а «нечего играть». Задача требует проверить именно отсутствие падения и
 * освобождение памяти.
 */
static void test_enqueue_zero_guard()
{
    std::printf("[TEST] 7в. Guard NULL/ноль: память освобождена, кодек молчит...\n");

    audio_mock_reset();
    audio_mock_set_delay_us(50 * 1000);
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "очередь пуста перед кейсом");

    // (а) NULL с владением: не падает и ничего не играет. Контракт master
    // (db4bd1b:171-178) — ESP_OK, а не ESP_ERR_INVALID_ARG: «нечего играть»
    // не ошибка вызывающего.
    CHECK(kouki_audio_out_enqueue_pcm(NULL, 0, true) == ESP_OK,
          "enqueue(NULL, 0, own) -> ESP_OK (как master)");
    CHECK(audio_mock_play_calls() == 0, "NULL не дошёл до кодека");
    CHECK(!kouki_audio_out_is_busy(), "после enqueue(NULL, 0) не «занят»");

    // (б) Нулевая длина с НЕнулевым буфером и владением: буфер обязан быть
    // освобождён. Без guard он утечёт: worker играет только элементы с bytes > 0
    // и free в этой ветке не выполнит, а класть в очередь нечего.
    void *warm = malloc(3200);
    free(warm);  // прогрев арены, чтобы дельта кучи считалась от стабильного дна

    const long before = heap_in_use();
    int16_t *zero = (int16_t *)malloc(3200);
    CHECK(zero != NULL, "буфер нулевой длины выделен");
    CHECK(kouki_audio_out_enqueue_pcm(zero, 0, true) == ESP_OK,
          "enqueue(bytes=0, own) -> ESP_OK");
    const long after = heap_in_use();

    CHECK(after - before <= 0,
          "буфер нулевой длины освобождён, а не утёк в очередь");
    CHECK(audio_mock_play_calls() == 0, "нулевая длина не играется");
    CHECK(!kouki_audio_out_is_busy(), "после enqueue(bytes=0) не «занят»");

    // (в) Guard не отбирает владение у НЕнулевого буфера: такой элемент
    // доигрывает и освобождается worker'ом.
    int16_t *real = (int16_t *)malloc(3200);
    CHECK(real != NULL, "обычный буфер выделен");
    real[0] = 5;
    CHECK(kouki_audio_out_enqueue_pcm(real, 3200, true) == ESP_OK, "enqueue обычного");
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "обычный доигран");
    CHECK(audio_mock_play_calls() == 1, "обычный элемент сыгран");
    audio_mock_set_delay_us(0);
}

/**
 * Пункт 10 ревью П5 (мутант a20): TONE_FADE_MS 5 -> 0.
 *
 * Fade делает перепад плавным: gain = i/fade для кадра i в начале блока и
 * (frames-1-i)/fade в конце, поэтому первые кадры почти нулевые, а последний
 * сэмпл ровно нулевой. Без fade (fade = 0) ветки `i < fade` не выполняются,
 * gain = 1.0, и первые кадры сразу идут на полной громкости синуса.
 *
 * Почему нельзя проверить «огибающую от наблюдаемого пика»: у START есть
 * затухание (decay_x10 = 6), поэтому пик блока приходится на t ≈ 0.7, а не
 * на начало, и линейная огибающая от пика ничего не ограничивает. Зато
 * отношение «первые кадры / пик блока» различает случаи на порядок:
 * с fade максимум первых восьми кадров — около 14 % пика (замер: 1494/10761),
 * без fade первый же кадр даёт ~35 % пика и быстро выходит на пик.
 *
 * Тонкости: блок СТЕРЕО (сэмплы чередуются L/R, кадр = 2 сэмпла), а первый
 * сэмпл у синуса и так ноль (sin(0) = 0) — поэтому смотрим окрестность, а не
 * только head[0]. Мок отдаёт крайние AUDIO_MOCK_EDGE_SAMPLES сэмплов и пик по
 * всему блоку (audio_mock.h).
 */
static void test_tone_fade()
{
    std::printf("[TEST] 10. Tone: плавный перепад на краях (fade)...\n");

#if TONE_EXPECT_VOLUME_0
    // При громкости 0 сигналов нет вовсе — fade проверять не на чем.
    std::printf("  (volume=0: fade не проверяется, сигналы отключены)\n");
#else
    audio_mock_reset();
    kouki_tone_play(KOUKI_TONE_START);

    CHECK(audio_mock_play_calls() >= 1, "START сыгран");
    const audio_mock_call_t *call = audio_mock_call(0);
    CHECK(call != NULL, "блок START записан");
    CHECK(call->head_frames > 0 && call->tail_frames > 0, "мок отдал края блока");
    CHECK(call->peak > 1000, "блок громкий: пик найден по всему блоку");

    // Крайние сэмплы блока — ровно нули: щелчка на границе нет.
    CHECK(call->head[0] == 0, "fade-in: первый сэмпл ровно ноль");
    CHECK(call->tail[call->tail_frames * 2 - 1] == 0, "fade-out: последний сэмпл ровно ноль");

    /*
     * Главная проверка: первые кадры заметно тише пика блока. Без fade они
     * сразу идут на полной громкости, и отношение близко к единице.
     */
    const int probe_frames = 8;
    CHECK(call->head_frames >= probe_frames, "мок запомнил достаточно кадров начала");
    int early_max = 0;
    for (int frame = 1; frame < probe_frames; frame++) {
        const int left = std::abs((int)call->head[frame * 2]);
        const int right = std::abs((int)call->head[frame * 2 + 1]);
        if (left > early_max) {
            early_max = left;
        }
        if (right > early_max) {
            early_max = right;
        }
    }
    const double ratio = (double)early_max / (double)call->peak;
    CHECK(ratio < 0.25, "fade-in: первые кадры тише четверти пика (иначе fade нет)");

    /*
     * Симметрично на выходе: кадры у конца блока тоже тише пика. С fade
     * последний сэмпл нулевой, предпоследние — единицы процентов пика; без
     * fade последний кадр равен обычному отсчёту синуса.
     */
    const int tail_frames = call->tail_frames;
    int late_max = 0;
    for (int k = 1; k < probe_frames; k++) {
        const int base = (tail_frames - 1 - k) * 2;
        const int left = std::abs((int)call->tail[base]);
        const int right = std::abs((int)call->tail[base + 1]);
        if (left > late_max) {
            late_max = left;
        }
        if (right > late_max) {
            late_max = right;
        }
    }
    const double late_ratio = (double)late_max / (double)call->peak;
    CHECK(late_ratio < 0.25, "fade-out: последние кадры тише четверти пика");
#endif
}

/**
 * Пункт 12 ревью П5 (мутант a17): abort() освобождает владеющие элементы.
 *
 * abort() вычерпывает очередь и для каждого Item с dynamically_allocated
 * освобождает data (app_audio_out.cpp:373-377). Мутант a17 убирает этот free:
 * очередь пустеет (счётчик и is_busy это не различают), но буферы утекают.
 * Поэтому проверка идёт по дельте занятой кучи, как в пункте 9.
 */
static void test_abort_frees_owned()
{
    std::printf("[TEST] 4б. abort: владеющие элементы очереди освобождены...\n");

    void *warm = malloc(3200);
    free(warm);  // прогрев арены

    const long before = heap_in_use();

    // Набиваем очередь элементами, которыми владеет очередь: их должен
    // освободить abort, а не вызывающий (владение уже передано).
    const int count = 5;
    for (int i = 0; i < count; i++) {
        int16_t *owned = (int16_t *)malloc(3200);
        CHECK(owned != NULL, "владеющий элемент выделен");
        owned[0] = (int16_t)i;
        CHECK(kouki_audio_out_enqueue_pcm(owned, 3200, true) == ESP_OK,
              "владеющий элемент поставлен в очередь");
    }
    CHECK(kouki_audio_out_is_busy(), "очередь непуста перед abort");

    kouki_audio_out_abort();
    /*
     * abort() вычерпывает очередь сразу, но worker мог уже взять блок и
     * отдавать его кодеку: is_busy() ещё какое-то время истинно. Дожидаемся
     * покоя, иначе тест ловит не утечку, а гонку с worker-ом.
     */
    CHECK(kouki_audio_out_wait_idle(portMAX_DELAY) == ESP_OK, "очередь разошлась после abort");
    const long after = heap_in_use();

    CHECK(!kouki_audio_out_is_busy(), "после abort очередь пуста");
    /*
     * На один-два элемента worker мог успеть отдать блок кодеку — их
     * освобождает ветка воспроизведения. Всё, что осталось в очереди,
     * обязан освободить abort. Без этого free прирост кучи близок к
     * count * 3200; с ним — нулевой или отрицательный.
     */
    CHECK(after - before <= 2 * 3200,
          "abort освободил владеющие элементы очереди (куча не выросла на всю очередь)");
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
    // Молчание проверяем для всех четырёх сигналов, а не только START:
    // каждый обязан выйти до синтеза и до подавления детектора.
    kouki_tone_play(KOUKI_TONE_WAKE);
    kouki_tone_play(KOUKI_TONE_STOP_SENDING);
    kouki_tone_play(KOUKI_TONE_ERROR);
    CHECK(audio_mock_play_calls() == 0, "volume=0: все четыре сигнала молчат");
    CHECK(g_wake_mute_calls == 0, "volume=0: детектор не подавляется ни разу");
    CHECK(g_wake_mute_last_ms == 0, "volume=0: длительность подавления даже не задана");
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
    test_abort_frees_owned();
    test_busy_idle();
    test_direct_play();
    test_enqueue_guards();
    test_enqueue_ownership_on_timeout();
    test_enqueue_zero_guard();
    test_tone_pattern();
    test_tone_fade();
    test_facade();

    std::printf("========================================\n");
    std::printf("ИТОГО: %d проверок успешно пройдено!\n", g_checks);
    std::printf("========================================\n");
    return 0;
}
