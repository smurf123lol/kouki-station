#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include <ctime>

// Include Station headers
#include "app_time.h"
#include "app_power.h"
#include "app_history.h"
#include "app_voice.h"
#include "fsm/voice_types.hpp"
#include "fsm/voice_context.hpp"
#include "fsm/voice_state_manager.hpp"
#include "fsm/voice_response_parser.hpp"
#include "fsm/states/idle_state.hpp"
#include "fsm/states/recording_state.hpp"
#include "fsm/states/sending_state.hpp"
#include "fsm/states/done_state.hpp"
#include "fsm/states/followup_state.hpp"
#include "fsm/states/error_state.hpp"

using namespace kouki::voice;

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")\n"; \
        std::exit(1); \
    } else { \
        g_tests_passed++; \
    } \
} while(0)

// -------------------------------------------------------------
// 1. Тест SNTP и форматирования времени (app_time)
// -------------------------------------------------------------
void test_time_module() {
    std::cout << "[TEST] 1. SNTP и форматирование времени (app_time)..." << std::endl;

    // Проверка установки fallback сервера
    kouki_time_set_fallback_server("192.168.1.1");

    // Инициализация с таймзоной MSK-3 (UTC+3)
    esp_err_t err = kouki_time_init("MSK-3");
    TEST_ASSERT(err == ESP_OK, "kouki_time_init returns ESP_OK");

    char hm_buf[16] = {0};
    kouki_time_format_hm(hm_buf, sizeof(hm_buf));
    TEST_ASSERT(hm_buf[0] != '\0', "kouki_time_format_hm returns non-empty string");

    char date_buf[64] = {0};
    kouki_time_format_date(date_buf, sizeof(date_buf));
    TEST_ASSERT(date_buf[0] != '\0', "kouki_time_format_date returns non-empty string");

    // Проверка структуры tm
    struct tm ti{};
    err = kouki_time_get(&ti);
    if (kouki_time_is_synced()) {
        TEST_ASSERT(err == ESP_OK, "kouki_time_get returns ESP_OK when synced");
        TEST_ASSERT(ti.tm_hour >= 0 && ti.tm_hour <= 23, "Valid hour range");
        TEST_ASSERT(ti.tm_min >= 0 && ti.tm_min <= 59, "Valid min range");
        TEST_ASSERT(ti.tm_mday >= 1 && ti.tm_mday <= 31, "Valid mday range");
        TEST_ASSERT(ti.tm_mon >= 0 && ti.tm_mon <= 11, "Valid mon range");
        TEST_ASSERT(ti.tm_wday >= 0 && ti.tm_wday <= 6, "Valid wday range");
    }

    std::cout << "       Формат HM: '" << hm_buf << "', Дата: '" << date_buf << "'" << std::endl;
}

// -------------------------------------------------------------
// 2. Тест VAD-метрик и снимка VoiceStateManager (app_voice / FSM)
// -------------------------------------------------------------
void test_vad_metrics() {
    std::cout << "[TEST] 2. Метрики VAD в snapshot (VoiceStateManager)..." << std::endl;

    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err_state;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err_state);

    std::vector<uint8_t> buffer(WAV_HEADER_SIZE + 48000, 0);
    mgr.context().buffer = buffer.data();
    mgr.context().capacity_bytes = 48000;

    TEST_ASSERT(mgr.init(nullptr, nullptr), "VoiceStateManager init");

    // Начальное состояние: IDLE
    kouki_voice_metrics_t m{};
    mgr.get_metrics(&m);
    TEST_ASSERT(!m.is_recording, "Idle: is_recording is false");
    TEST_ASSERT(!m.speech_detected, "Idle: speech_detected is false");
    TEST_ASSERT(m.silence_ms == 0, "Idle: silence_ms is 0");
    TEST_ASSERT(m.record_duration_ms == 0, "Idle: record_duration_ms is 0");
    TEST_ASSERT(m.silence_limit_ms == 3000, "Idle: silence_limit_ms is 3000");

    // Переход в RECORDING
    mgr.transition_to(StateId::Recording, "button_start");
    TEST_ASSERT(mgr.current_id() == StateId::Recording, "Now in Recording state");

    mgr.get_metrics(&m);
    TEST_ASSERT(m.is_recording, "Recording: is_recording is true");
    TEST_ASSERT(!m.speech_detected, "Recording: speech_detected initially false");
    TEST_ASSERT(m.silence_ms == 0, "Recording: silence_ms is 0 before speech starts");

    // 1 чанк тишины (512 сэмплов = 32 мс)
    int16_t quiet_pcm[512] = {0};
    // Подаем 10 чанков тишины (320 мс)
    for (int i = 0; i < 10; i++) {
        mgr.step(quiet_pcm, 512);
    }
    mgr.get_metrics(&m);
    TEST_ASSERT(m.is_recording, "Still in Recording");
    TEST_ASSERT(!m.speech_detected, "Speech still not detected (quiet)");
    TEST_ASSERT(m.silence_ms == 0, "silence_ms MUST be 0 when speech_detected == false");
    TEST_ASSERT(m.record_duration_ms > 0, "record_duration_ms increments");

    // Подаем громкий звук (речь началась)
    int16_t loud_pcm[512];
    for (int i = 0; i < 512; i++) {
        loud_pcm[i] = (i % 2 == 0) ? 2500 : -2500;
    }
    mgr.step(loud_pcm, 512);

    mgr.get_metrics(&m);
    TEST_ASSERT(m.speech_detected, "speech_detected is now true after loud sound");
    TEST_ASSERT(m.silence_ms == 0, "silence_ms is 0 while speaking");

    // Теперь подаем тишину после речи
    for (int i = 0; i < 5; i++) {
        mgr.step(quiet_pcm, 512);
    }
    mgr.get_metrics(&m);
    TEST_ASSERT(m.speech_detected, "speech_detected remains true");
    TEST_ASSERT(m.silence_ms > 0, "silence_ms now counts silence after speech detected");

    // Снова громкий звук — тишина должна сброситься в 0
    mgr.step(loud_pcm, 512);
    mgr.get_metrics(&m);
    TEST_ASSERT(m.silence_ms == 0, "silence_ms resets to 0 when speech resumes");

    // Проверка режима followup (dialogue_continue)
    mgr.transition_to(StateId::Idle, "reset");
    mgr.context().dialogue_continue = true;
    mgr.transition_to(StateId::Recording, "followup_speech");
    mgr.get_metrics(&m);
    TEST_ASSERT(m.speech_detected == true, "speech_detected initialized to true for followup recording");
    TEST_ASSERT(m.silence_limit_ms == 1500, "silence_limit_ms is 1500 for followup recording");
}

// -------------------------------------------------------------
// 3. Тест диспетчера питания и димминга (app_power)
// -------------------------------------------------------------
void test_power_dimming() {
    std::cout << "[TEST] 3. Диспетчер питания и димминг (app_power)..." << std::endl;

    kouki_power_init();
    TEST_ASSERT(kouki_power_get_brightness() == 100, "Initial brightness is 100%");
    TEST_ASSERT(kouki_power_get_idle_ms() == 0, "Initial idle time is 0");

    // Тикаем 30 секунд (в пределах 45 с покое)
    for (int i = 0; i < 30; i++) {
        kouki_power_tick(1000);
    }
    TEST_ASSERT(kouki_power_get_idle_ms() == 30000, "Idle time is 30000 ms");
    TEST_ASSERT(kouki_power_get_brightness() == 100, "Brightness remains 100% at 30s");

    // Тикаем еще 16 секунд (итого 46 с — порог димминга 45 с преодолен)
    for (int i = 0; i < 16; i++) {
        kouki_power_tick(1000);
    }
    TEST_ASSERT(kouki_power_get_idle_ms() == 46000, "Idle time is 46000 ms");
    // Димминг до 20% занимает 1.5 с, через 16 секунд точно достиг 20%
    TEST_ASSERT(kouki_power_get_brightness() == 20, "Brightness dimmed to 20% after 45s");

    // Тикаем до 179 секунд (до 180 с держится 20%)
    while (kouki_power_get_idle_ms() < 179000) {
        kouki_power_tick(1000);
    }
    TEST_ASSERT(kouki_power_get_brightness() == 20, "Brightness remains 20% before 180s");

    // Тикаем до 185 секунд (порог 180 с преодолен, затухание в 0% за 1 с)
    for (int i = 0; i < 6; i++) {
        kouki_power_tick(1000);
    }
    TEST_ASSERT(kouki_power_get_idle_ms() >= 185000, "Idle time >= 185000 ms");
    TEST_ASSERT(kouki_power_get_brightness() == 0, "Brightness is 0% after 180s timeout");

    // Проверка насыщения s_idle_ms для защиты от переполнения
    for (int i = 0; i < 50; i++) {
        kouki_power_tick(1000);
    }
    TEST_ASSERT(kouki_power_get_idle_ms() <= 186000, "Idle time saturates to prevent uint32 overflow");

    // Событие активности (тач или wake-word)
    kouki_power_notify_activity();
    TEST_ASSERT(kouki_power_get_idle_ms() == 0, "Idle time reset to 0 after activity");
    TEST_ASSERT(kouki_power_get_brightness() == 100, "Brightness instantly restored to 100%");
}

// -------------------------------------------------------------
// 4. Тест кольцевого буфера истории (app_history)
// -------------------------------------------------------------
void test_history_ring_buffer() {
    std::cout << "[TEST] 4. Кольцевой буфер истории в PSRAM (app_history)..." << std::endl;

    kouki_history_init();
    kouki_history_clear();
    TEST_ASSERT(kouki_history_count() == 0, "History initially empty");
    const uint32_t v_init = kouki_history_version();

    kouki_history_item_t item{};
    TEST_ASSERT(!kouki_history_get(0, &item), "get(0) on empty returns false");

    // Добавляем 12 результатов в буфер емкостью 10
    for (int i = 1; i <= 12; i++) {
        kouki_voice_result_t res{};
        std::snprintf(res.transcript, sizeof(res.transcript), "Запрос %d", i);
        std::snprintf(res.task_title, sizeof(res.task_title), "Задача %d", i);
        std::snprintf(res.answer, sizeof(res.answer), "Ответ %d", i);
        std::snprintf(res.outcome, sizeof(res.outcome), "created");
        std::strcpy(res.intent, "add_task");
        kouki_history_push(&res);
    }

    // Емкость ограничена 10
    TEST_ASSERT(kouki_history_count() == 10, "History capacity clamped at 10");
    TEST_ASSERT(kouki_history_version() == v_init + 12, "History version increments once per push");

    // index 0 — самый свежий (Запрос 12)
    TEST_ASSERT(kouki_history_get(0, &item), "get(0) succeeds");
    TEST_ASSERT(std::strcmp(item.transcript, "Запрос 12") == 0, "index 0 is newest ('Запрос 12')");
    TEST_ASSERT(std::strcmp(item.title, "Задача 12") == 0, "index 0 title matches");
    TEST_ASSERT(item.is_task == true, "item 12 marked as task");
    TEST_ASSERT(item.is_answer == true, "item 12 marked as answer");

    // index 1 — предпоследний (Запрос 11)
    TEST_ASSERT(kouki_history_get(1, &item), "get(1) succeeds");
    TEST_ASSERT(std::strcmp(item.transcript, "Запрос 11") == 0, "index 1 is 'Запрос 11'");

    // index 9 — самый старый оставшийся (Запрос 3, так как 1 и 2 вытеснены)
    TEST_ASSERT(kouki_history_get(9, &item), "get(9) succeeds");
    TEST_ASSERT(std::strcmp(item.transcript, "Запрос 3") == 0, "index 9 is oldest remaining ('Запрос 3')");

    // index 10 — вне диапазона
    TEST_ASSERT(!kouki_history_get(10, &item), "get(10) returns false");

    // Очистка
    kouki_history_clear();
    TEST_ASSERT(kouki_history_count() == 0, "History count is 0 after clear");
    TEST_ASSERT(kouki_history_version() == v_init + 13, "History version increments on clear");
    TEST_ASSERT(!kouki_history_get(0, &item), "get(0) after clear returns false");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "    HOST CHECK: T114 Platform Behavior  " << std::endl;
    std::cout << "========================================" << std::endl;

    test_time_module();
    test_vad_metrics();
    test_power_dimming();
    test_history_ring_buffer();

    std::cout << "========================================" << std::endl;
    std::cout << "ИТОГО: " << g_tests_passed << "/" << g_tests_run << " проверок успешно пройдено!" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
