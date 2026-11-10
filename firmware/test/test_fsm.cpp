#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>

#include "app_history.h"
#include "app_vad.h"
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

void test_initialization() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = 48000;

    bool ok = mgr.init(nullptr, nullptr);
    TEST_ASSERT(ok, "Manager init success");
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Initial state is Idle");
    TEST_ASSERT(std::strcmp(mgr.current_name(), "IDLE") == 0, "Initial state name is IDLE");

    FsmDiagnostics d = mgr.get_diagnostics();
    TEST_ASSERT(d.current_id == StateId::Idle, "Diag current is Idle");
    TEST_ASSERT(d.transition_count == 0, "Diag transition count 0");
    TEST_ASSERT(d.interrupt_count == 0, "Diag interrupt count 0");
    std::cout << "  [PASS] test_initialization\n";
}

void test_guard_conditions() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);
    mgr.init(nullptr, nullptr);

    // 1) From Idle
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Idle, StateId::Recording), "Idle -> Recording allowed");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Idle, StateId::Done), "Idle -> Done rejected");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Idle, StateId::Sending), "Idle -> Sending rejected");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Idle, StateId::Followup), "Idle -> Followup rejected");

    // 2) From Recording
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Recording, StateId::Sending), "Recording -> Sending allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Recording, StateId::Idle), "Recording -> Idle allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Recording, StateId::Error), "Recording -> Error allowed");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Recording, StateId::Done), "Recording -> Done rejected");

    // 3) From Sending
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Sending, StateId::Done), "Sending -> Done allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Sending, StateId::Error), "Sending -> Error allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Sending, StateId::Idle), "Sending -> Idle allowed");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Sending, StateId::Recording), "Sending -> Recording rejected");

    // 4) From Done
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Done, StateId::Followup), "Done -> Followup allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Done, StateId::Idle), "Done -> Idle allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Done, StateId::Recording), "Done -> Recording allowed on button");

    // 5) From Followup
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Followup, StateId::Recording), "Followup -> Recording allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Followup, StateId::Idle), "Followup -> Idle allowed");
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Followup, StateId::Error), "Followup -> Error allowed");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Followup, StateId::Sending), "Followup -> Sending rejected");

    // 6) From Error
    TEST_ASSERT(mgr.is_transition_allowed(StateId::Error, StateId::Idle), "Error -> Idle allowed");
    TEST_ASSERT(!mgr.is_transition_allowed(StateId::Error, StateId::Sending), "Error -> Sending rejected");

    // Test transition attempt to illegal state: should remain Idle
    mgr.transition_to(StateId::Done, "illegal_attempt");
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Remains Idle after illegal transition");

    std::cout << "  [PASS] test_guard_conditions\n";
}

void test_interruption_lifecycle() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = 48000;
    mgr.init(nullptr, nullptr);

    // 1) Transition Idle -> Recording via button API, record loud speech, stop via button API
    TEST_ASSERT(mgr.start_recording_from_button(), "start_recording_from_button works in Idle");
    TEST_ASSERT(mgr.current_id() == StateId::Recording, "Entered Recording");

    int16_t loud_pcm[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) loud_pcm[i] = 1000;
    for (int c = 0; c < 6; c++) {
        mgr.step(loud_pcm, RECORD_CHUNK);
    }

    // 2) Interrupt button_stop -> should set stop_requested and move Recording -> Sending
    mgr.interrupt("button_stop");
    TEST_ASSERT(mgr.context().stop_requested, "Stop requested set via interrupt");

    int16_t dummy_pcm[1024] = {0};
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Transitioned to Sending on stop_requested");

    // 3) In Sending: button_start / button_stop / stop_and_send / set_session MUST NOT cancel upload
    mgr.interrupt("button_start");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Sending ignores button_start interrupt");
    mgr.interrupt("button_stop");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Sending ignores button_stop interrupt");
    TEST_ASSERT(!mgr.start_recording_from_button(), "start_recording_from_button refused in Sending");
    TEST_ASSERT(!mgr.stop_and_send_from_button(), "stop_and_send_from_button does not cancel Sending");
    TEST_ASSERT(!mgr.try_set_session(KOUKI_SESSION_ASK), "try_set_session refused in Sending");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Still in Sending after button attempts");

    // 4) Interrupt cancel while sending -> should abort and return to Idle
    mgr.interrupt("cancel");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Returned to Idle on cancel in Sending");

    // 5) Worker completion in Sending moves Sending -> Done, and quiet audio moves Sending -> Error
    TEST_ASSERT(mgr.start_recording_from_button(), "Re-entered Recording");
    for (int c = 0; c < 6; c++) {
        mgr.step(loud_pcm, RECORD_CHUNK);
    }
    TEST_ASSERT(mgr.stop_and_send_from_button(), "stop_and_send_from_button accepted in Recording");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Moved to Sending after stop_and_send");
    send.notify_complete(StateId::Done);
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Done, "Sending moves to Done on worker completion");
    mgr.transition_to(StateId::Idle, "cleanup");

    TEST_ASSERT(mgr.start_recording_from_button(), "Enter Recording for quiet guard check");
    mgr.step(dummy_pcm, RECORD_CHUNK);
    TEST_ASSERT(mgr.stop_and_send_from_button(), "Stop quiet Recording");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Entered Sending with quiet buffer");
    mgr.step(dummy_pcm, 1024);
    TEST_ASSERT(mgr.current_id() == StateId::Error, "Quiet buffer moves Sending -> Error");
    mgr.transition_to(StateId::Idle, "cleanup_err");

    FsmDiagnostics d = mgr.get_diagnostics();
    TEST_ASSERT(d.interrupt_count == 6, "6 interrupts recorded (4 explicit + 2 stop_and_send)");

    std::cout << "  [PASS] test_interruption_lifecycle\n";
}

void test_vad_silence_3000ms() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 480000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = 480000;
    mgr.init(nullptr, nullptr);

    // Входим в запись через wake word
    mgr.context().is_wake_recording = true;
    mgr.transition_to(StateId::Recording, "wake_word");

    int16_t loud_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) loud_chunk[i] = 1000; // Громкий звук

    int16_t silence_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) silence_chunk[i] = 20; // Тишина

    // Первые 5 чанков (320 мс) игнорируются для тона
    for (int c = 0; c < 5; c++) {
        mgr.step(loud_chunk, RECORD_CHUNK);
        TEST_ASSERT(mgr.current_id() == StateId::Recording, "Still recording during initial tone window");
    }

    // Подаем речь
    mgr.step(loud_chunk, RECORD_CHUNK);
    TEST_ASSERT(mgr.context().speech_detected, "Speech detected by VAD");

    // Теперь подаем тишину до срабатывания VAD отсечки (3000 мс)
    while (mgr.current_id() == StateId::Recording) {
        mgr.step(silence_chunk, RECORD_CHUNK);
    }
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Transitioned to Sending after 3000ms silence");
    TEST_ASSERT(mgr.context().last_vad_silence_ms >= 3000, "VAD silence reached at least 3000ms");

    std::cout << "  [PASS] test_vad_silence_3000ms\n";
}

void test_response_parser() {
    const char *sample_json =
        "{\n"
        "  \"transcript\": \"сколько времени\",\n"
        "  \"intake\": {\n"
        "    \"intent\": {\"intent\": \"ask\"},\n"
        "    \"outcome\": \"executed\",\n"
        "    \"command\": \"ask\",\n"
        "    \"task\": {\"title\": \"сколько времени\", \"status\": \"done\"},\n"
        "    \"result\": {\"answer\": \"Сейчас 16:35\", \"requiresConfirmation\": true}\n"
        "  }\n"
        "}";

    kouki_voice_result_t res{};
    bool ok = VoiceResponseParser::parse(sample_json, &res);
    TEST_ASSERT(ok, "Parse success");
    TEST_ASSERT(res.has_transcript, "has_transcript true");
    TEST_ASSERT(std::strcmp(res.transcript, "сколько времени") == 0, "Transcript matches");
    TEST_ASSERT(std::strcmp(res.intent, "ask") == 0, "Intent matches");
    TEST_ASSERT(std::strcmp(res.outcome, "executed") == 0, "Outcome matches");
    TEST_ASSERT(std::strcmp(res.command, "ask") == 0, "Command matches");
    TEST_ASSERT(std::strcmp(res.task_title, "сколько времени") == 0, "Task title matches");
    TEST_ASSERT(std::strcmp(res.answer, "Сейчас 16:35") == 0, "Answer matches");
    TEST_ASSERT(res.requires_confirmation == true, "requiresConfirmation true");

    // Тест ошибки
    const char *err_json = "{\"error\": \"Нет связи с сервером\"}";
    kouki_voice_result_t err_res{};
    VoiceResponseParser::parse(err_json, &err_res);
    TEST_ASSERT(std::strcmp(err_res.error, "Нет связи с сервером") == 0, "Error parsed");

    // Тест невалидного JSON
    kouki_voice_result_t bad_res{};
    bool bad_ok = VoiceResponseParser::parse("NOT_A_JSON", &bad_res);
    TEST_ASSERT(!bad_ok || bad_res.error[0] != '\0', "Invalid JSON flagged");

    std::cout << "  [PASS] test_response_parser\n";
}

void test_button_vad_silence_3000ms() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 480000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = 480000;
    mgr.init(nullptr, nullptr);

    // Старт по кнопке: is_wake_recording = false!
    mgr.context().is_wake_recording = false;
    mgr.transition_to(StateId::Recording, "button_start");

    int16_t loud_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) loud_chunk[i] = 1000;

    int16_t silence_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) silence_chunk[i] = 20;

    // Первые 5 чанков (320 мс)
    for (int c = 0; c < 5; c++) {
        mgr.step(loud_chunk, RECORD_CHUNK);
        TEST_ASSERT(mgr.current_id() == StateId::Recording, "Button: still recording during tone window");
    }

    // Речь
    mgr.step(loud_chunk, RECORD_CHUNK);
    TEST_ASSERT(mgr.context().speech_detected, "Button: speech detected by VAD");

    // Теперь подаем тишину до срабатывания VAD отсечки (3000 мс)
    while (mgr.current_id() == StateId::Recording) {
        mgr.step(silence_chunk, RECORD_CHUNK);
    }
    TEST_ASSERT(mgr.current_id() == StateId::Sending, "Button: transitioned to Sending after 3000ms silence");
    TEST_ASSERT(mgr.context().last_vad_silence_ms >= 3000, "Button: VAD silence reached at least 3000ms");

    std::cout << "  [PASS] test_button_vad_silence_3000ms\n";
}

void test_done_speech_interruption() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 480000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = 480000;
    mgr.init(nullptr, nullptr);

    mgr.transition_to(StateId::Recording, "start");
    mgr.transition_to(StateId::Sending, "done_rec");
    mgr.transition_to(StateId::Done, "http_ok");
    TEST_ASSERT(mgr.current_id() == StateId::Done, "In Done state");

    // Во время речи нажимаем "Слушать" (button_start)
    mgr.interrupt("button_start");
    mgr.step(nullptr, 0);
    // Должен сразу перейти в RECORDING!
    TEST_ASSERT(mgr.current_id() == StateId::Recording, "Done interrupted by button_start goes to Recording");

    // Повторно входим в Done и проверяем прерывание cancel -> Idle
    mgr.transition_to(StateId::Sending, "done_rec_2");
    mgr.transition_to(StateId::Done, "http_ok_2");
    TEST_ASSERT(mgr.current_id() == StateId::Done, "Re-entered Done state");
    mgr.interrupt("cancel");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Done interrupted by cancel returns to Idle");

    std::cout << "  [PASS] test_done_speech_interruption\n";
}

void test_context_resets_and_dialogue_cleanup() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = 48000;
    mgr.init(nullptr, nullptr);

    // 1. reset_recording() обязан сбрасывать followup_silence_chunks (исправляет утечку счетчика между ходами)
    mgr.context().followup_silence_chunks = 20;
    mgr.context().reset_recording();
    TEST_ASSERT(mgr.context().followup_silence_chunks == 0, "reset_recording clears followup_silence_chunks");

    // 2. IdleState должен сбрасывать застрявший флаг dialogue_continue и при входе, и при button_start
    mgr.context().dialogue_continue = true;
    mgr.interrupt("button_start");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Recording, "Idle -> Recording on button_start");
    TEST_ASSERT(mgr.context().dialogue_continue == false, "button_start from Idle clears dialogue_continue");

    // 3. ErrorState обязан очищать dialogue_continue при входе
    mgr.transition_to(StateId::Error, "sim_err");
    TEST_ASSERT(mgr.current_id() == StateId::Error, "In Error state");
    mgr.context().dialogue_continue = true;
    mgr.transition_to(StateId::Idle, "back_idle");
    TEST_ASSERT(mgr.context().dialogue_continue == false, "Idle::on_enter clears dialogue_continue");

    mgr.transition_to(StateId::Recording, "r2");
    mgr.context().dialogue_continue = true;
    mgr.transition_to(StateId::Error, "err2");
    TEST_ASSERT(mgr.context().dialogue_continue == false, "Error::on_enter clears dialogue_continue");

    std::cout << "  [PASS] test_context_resets_and_dialogue_cleanup\n";
}

void test_followup_speech_streak_guard() {
    FollowupState follow;
    VoiceContext ctx{};
    ctx.dialogue_continue = true;

    follow.on_enter(ctx, "done");
    TEST_ASSERT(ctx.dialogue_continue == false, "Followup::on_enter clears dialogue_continue");
    TEST_ASSERT(follow.loud_streak() == 0, "Followup::on_enter resets loud_streak");

    int16_t noise_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) noise_chunk[i] = (i % 2 == 0) ? 1000 : -1000;

    int16_t quiet_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) quiet_chunk[i] = 20;

    int16_t speech_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) speech_chunk[i] = 1000;

    kouki_vad_reset();

    // Шум НЕ увеличивает streak (VAD отсекает шум)
    kouki_vad_host_set_mock(true, false);
    TEST_ASSERT(!follow.evaluate_speech_chunk(noise_chunk, RECORD_CHUNK, 0), "Noise rejected by VAD");
    TEST_ASSERT(follow.loud_streak() == 0, "Streak is 0 on noise");
    kouki_vad_host_set_mock(false, false);

    // 1 чанк речи (1024 сэмпла = 2 фрейма по 30 мс) еще недостаточно для подтверждения (нужно 3 фрейма = 90 мс)
    TEST_ASSERT(!follow.evaluate_speech_chunk(speech_chunk, RECORD_CHUNK, 0), "1 speech chunk (2 frames) not enough");
    TEST_ASSERT(follow.loud_streak() == 2, "Streak reached 2 frames");

    // Пауза сбрасывает streak обратно в 0
    TEST_ASSERT(!follow.evaluate_speech_chunk(quiet_chunk, RECORD_CHUNK, 0), "Quiet chunk resets streak");
    TEST_ASSERT(follow.loud_streak() == 0, "Streak reset to 0 on quiet chunk");

    // Прерывистые щелчки и шум тоже сбрасывают
    TEST_ASSERT(!follow.evaluate_speech_chunk(speech_chunk, RECORD_CHUNK, 0), "Speech chunk 1");
    TEST_ASSERT(follow.loud_streak() == 2, "Streak is 2");
    kouki_vad_host_set_mock(true, false);
    TEST_ASSERT(!follow.evaluate_speech_chunk(noise_chunk, RECORD_CHUNK, 0), "Noise resets streak");
    TEST_ASSERT(follow.loud_streak() == 0, "Streak is 0 on noise");
    kouki_vad_host_set_mock(false, false);

    // С T119 onset follow-up требует 13 фреймов VAD (390 мс), а не 3:
    // один чанк 1024 сэмпла даёт 2–3 фрейма (30 мс), поэтому срабатывание
    // наступает не раньше 5-го чанка и не позже 8-го.
    int trigger_chunk = -1;
    for (int i = 0; i < 8; ++i) {
        if (follow.evaluate_speech_chunk(speech_chunk, RECORD_CHUNK, 0)) {
            trigger_chunk = i;
            break;
        }
    }
    TEST_ASSERT(trigger_chunk >= 4, "Onset fires no earlier than the 5th chunk (13 frames)");
    TEST_ASSERT(trigger_chunk >= 0, "Onset fires within 8 speech chunks");
    TEST_ASSERT(follow.loud_streak() >= 13, "Streak reached >= 13 frames at onset");

    std::cout << "  [PASS] test_followup_speech_streak_guard\n";
}

void test_history_version_monotonic() {
    kouki_history_init();
    kouki_history_clear();
    const uint32_t v0 = kouki_history_version();

    kouki_voice_result_t res{};
    std::strcpy(res.transcript, "Привет Куки");
    kouki_history_push(&res);
    const uint32_t v1 = kouki_history_version();
    TEST_ASSERT(v1 == v0 + 1, "kouki_history_version increments on push");

    kouki_history_clear();
    const uint32_t v2 = kouki_history_version();
    TEST_ASSERT(v2 == v1 + 1, "kouki_history_version increments on clear");

    std::cout << "  [PASS] test_history_version_monotonic\n";
}


void test_t116_fsm_improvements() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = sizeof(buffer);
    mgr.init(nullptr, nullptr);

    // 1. get_snapshot returns bool
    kouki_voice_state_t v_state{};
    kouki_voice_result_t v_res{};
    bool snap_ok = mgr.get_snapshot(&v_state, &v_res);
    TEST_ASSERT(snap_ok, "get_snapshot returns true");
    TEST_ASSERT(v_state == KOUKI_VOICE_IDLE, "Initial state is IDLE");

    // 2. dismiss_error() from StateId::Error transitions to StateId::Idle
    // To enter Error cleanly: Idle -> Recording -> Error
    mgr.transition_to(StateId::Recording, "to_rec");
    mgr.transition_to(StateId::Error, "to_err");
    TEST_ASSERT(mgr.current_id() == StateId::Error, "Current state is Error");
    bool dismiss_ok = mgr.dismiss_error();
    TEST_ASSERT(dismiss_ok, "dismiss_error returned true");
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "dismiss_error transitioned to Idle");

    // 3. start_recording_from_button() from StateId::Error transitions to Recording
    mgr.transition_to(StateId::Recording, "to_rec2");
    mgr.transition_to(StateId::Error, "to_err2");
    TEST_ASSERT(mgr.current_id() == StateId::Error, "In Error state");
    mgr.start_recording_from_button();
    TEST_ASSERT(mgr.current_id() == StateId::Recording, "start_recording_from_button from Error enters Recording");

    // 4. VoiceResponseParser::parse with UTF-8 truncation boundary
    std::string long_answer;
    for (int i = 0; i < 400; i++) {
        long_answer += "Привет! ";
    }
    std::string json_str = std::string("{\"intake\": {\"result\": {\"answer\": \"") + long_answer + std::string("\"}}}");
    kouki_voice_result_t utf8_res{};
    bool parse_ok = VoiceResponseParser::parse(json_str.c_str(), &utf8_res);
    TEST_ASSERT(parse_ok, "Parsed long cyrillic JSON response");
    size_t ans_len = std::strlen(utf8_res.answer);
    TEST_ASSERT(ans_len < sizeof(utf8_res.answer), "Answer fits inside buffer");
    // Verify last byte is not an incomplete UTF-8 lead byte (>= 0xC0)
    unsigned char last_byte = static_cast<unsigned char>(utf8_res.answer[ans_len - 1]);
    TEST_ASSERT(last_byte < 0x80 || (last_byte >= 0x80 && (last_byte & 0xC0) == 0x80),
                "Last byte of truncated answer is valid UTF-8 continuation or ASCII");

    std::cout << "  [PASS] test_t116_fsm_improvements\n";
}

void test_t117_vad_filter_speech_vs_noise() {
    kouki_vad_init();
    kouki_vad_reset();

    int16_t silence[480];
    for (int i = 0; i < 480; i++) silence[i] = 20;

    int16_t speech[480];
    for (int i = 0; i < 480; i++) speech[i] = 1000;

    // 1. Тишина дает non-speech
    bool sp1 = kouki_vad_process(silence, 480);
    TEST_ASSERT(!sp1, "Silence classified as non-speech");
    TEST_ASSERT(kouki_vad_get_speech_streak() == 0, "Streak 0 on silence");

    // 2. Речь дает speech и накапливает streak
    bool sp3 = kouki_vad_process(speech, 480);
    TEST_ASSERT(sp3, "Speech frame classified as speech");
    TEST_ASSERT(kouki_vad_get_speech_streak() == 1, "Streak is 1 after 1 speech frame");

    bool sp4 = kouki_vad_process(speech, 480);
    TEST_ASSERT(sp4, "Second speech frame classified as speech");
    TEST_ASSERT(kouki_vad_get_speech_streak() == 2, "Streak is 2 after 2 speech frames");

    // 3. Появление тишины мгновенно сбрасывает streak в 0
    bool sp5 = kouki_vad_process(silence, 480);
    TEST_ASSERT(!sp5, "Silence resets speech streak");
    TEST_ASSERT(kouki_vad_get_speech_streak() == 0, "Streak reset to 0");

    // 5. Мок VAD
    kouki_vad_host_set_mock(true, true);
    TEST_ASSERT(kouki_vad_process(silence, 480), "Mock forces speech");
    kouki_vad_host_set_mock(true, false);
    TEST_ASSERT(!kouki_vad_process(speech, 480), "Mock forces silence");
    kouki_vad_host_set_mock(false, false);

    std::cout << "  [PASS] test_t117_vad_filter_speech_vs_noise\n";
}

void test_t117_followup_noise_rejection() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = sizeof(buffer);
    mgr.init(nullptr, nullptr);

    mgr.transition_to(StateId::Recording, "to_rec");
    mgr.transition_to(StateId::Sending, "to_send");
    mgr.transition_to(StateId::Done, "to_done");
    std::strcpy(mgr.context().result.transcript, "тест");
    std::strcpy(mgr.context().result.answer, "ответ");
    std::strcpy(mgr.context().result.intent, "ask");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Followup, "Transitioned to Followup from Done");

    // Подаем 20 чанков шума (фон ТВ/музыка, VAD не детектирует речь)
    kouki_vad_host_set_mock(true, false);
    int16_t noise_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) noise_chunk[i] = (i % 2 == 0) ? 1200 : -1200;

    for (int i = 0; i < 20; i++) {
        mgr.step(noise_chunk, RECORD_CHUNK);
        TEST_ASSERT(mgr.current_id() == StateId::Followup, "Noise does NOT trigger Recording from Followup");
    }
    kouki_vad_host_set_mock(false, false);

    std::cout << "  [PASS] test_t117_followup_noise_rejection\n";
}

void test_t117_recording_followup_abort_on_noise() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 480000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = sizeof(buffer);
    mgr.init(nullptr, nullptr);

    // Входим в запись из follow-up (dialogue_continue = true)
    mgr.context().dialogue_continue = true;
    mgr.transition_to(StateId::Recording, "followup_speech");
    TEST_ASSERT(mgr.current_id() == StateId::Recording, "In Recording state with dialogue_continue");

    // Подаем шум (VAD не детектирует речь человека)
    kouki_vad_host_set_mock(true, false);
    int16_t noise_chunk[RECORD_CHUNK];
    for (size_t i = 0; i < RECORD_CHUNK; i++) noise_chunk[i] = (i % 2 == 0) ? 1000 : -1000;

    // Шагаем шумом: за первые 2500 мс (или по отсечке тишины 1500 мс) речь не подтвердилась VAD ->
    // запись должна локально отмениться в Idle!
    for (int i = 0; i < 45; i++) {
        mgr.step(noise_chunk, RECORD_CHUNK);
        if (mgr.current_id() == StateId::Idle) {
            break;
        }
    }
    kouki_vad_host_set_mock(false, false);

    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Recording aborted to IDLE after unconfirmed noise in follow-up");

    std::cout << "  [PASS] test_t117_recording_followup_abort_on_noise\n";
}

void test_t117_done_streak_limit() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = sizeof(buffer);
    mgr.init(nullptr, nullptr);

    // Шаг 1: Done -> Followup (streak=1)
    mgr.transition_to(StateId::Recording, "rec1");
    mgr.transition_to(StateId::Sending, "send1");
    mgr.transition_to(StateId::Done, "done1");
    std::strcpy(mgr.context().result.transcript, "вопрос 1");
    std::strcpy(mgr.context().result.answer, "ответ 1");
    std::strcpy(mgr.context().result.intent, "ask");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Followup, "Done step 1 -> Followup");
    TEST_ASSERT(mgr.context().followup_streak == 1, "Streak is 1");

    // Шаг 2: Followup -> Recording -> Sending -> Done -> Followup (streak=2)
    mgr.transition_to(StateId::Recording, "rec2");
    mgr.transition_to(StateId::Sending, "send2");
    mgr.transition_to(StateId::Done, "done2");
    std::strcpy(mgr.context().result.transcript, "вопрос 2");
    std::strcpy(mgr.context().result.answer, "ответ 2");
    std::strcpy(mgr.context().result.intent, "ask");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Followup, "Done step 2 -> Followup");
    TEST_ASSERT(mgr.context().followup_streak == 2, "Streak is 2");

    // Шаг 3: Followup -> Recording -> Sending -> Done -> Followup (streak=3)
    mgr.transition_to(StateId::Recording, "rec3");
    mgr.transition_to(StateId::Sending, "send3");
    mgr.transition_to(StateId::Done, "done3");
    std::strcpy(mgr.context().result.transcript, "вопрос 3");
    std::strcpy(mgr.context().result.answer, "ответ 3");
    std::strcpy(mgr.context().result.intent, "ask");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Followup, "Done step 3 -> Followup");
    TEST_ASSERT(mgr.context().followup_streak == 3, "Streak is 3");

    // Шаг 4: Followup -> Recording -> Sending -> Done -> лимит 3 исчерпан -> выход в IDLE!
    mgr.transition_to(StateId::Recording, "rec4");
    mgr.transition_to(StateId::Sending, "send4");
    mgr.transition_to(StateId::Done, "done4");
    std::strcpy(mgr.context().result.transcript, "вопрос 4");
    std::strcpy(mgr.context().result.answer, "ответ 4");
    std::strcpy(mgr.context().result.intent, "ask");
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Done step 4 exceeds limit 3 -> transitions to IDLE");
    TEST_ASSERT(mgr.context().followup_streak == 0, "Streak reset to 0 upon reaching limit");

    std::cout << "  [PASS] test_t117_done_streak_limit\n";
}

void test_t117_done_empty_or_error_rejects_followup() {
    VoiceStateManager mgr;
    IdleState idle;
    RecordingState rec;
    SendingState send;
    DoneState done;
    FollowupState follow;
    ErrorState err;

    mgr.register_state(&idle);
    mgr.register_state(&rec);
    mgr.register_state(&send);
    mgr.register_state(&done);
    mgr.register_state(&follow);
    mgr.register_state(&err);

    uint8_t buffer[WAV_HEADER_SIZE + 48000];
    mgr.context().buffer = buffer;
    mgr.context().capacity_bytes = sizeof(buffer);
    mgr.init(nullptr, nullptr);

    // 1. Пустой транскрипт (станция услышала только тишину/шум) -> выход в IDLE без follow-up
    mgr.transition_to(StateId::Recording, "rec_empty");
    mgr.transition_to(StateId::Sending, "send_empty");
    mgr.transition_to(StateId::Done, "done_empty");
    mgr.context().result.transcript[0] = '\0';
    std::strcpy(mgr.context().result.answer, "Ответ");
    std::strcpy(mgr.context().result.intent, "ask");
    mgr.context().followup_streak = 2;
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Empty transcript exits to IDLE");
    TEST_ASSERT(mgr.context().followup_streak == 0, "Streak reset on empty transcript");

    // 2. Ошибка сервера/сети -> выход в IDLE без follow-up
    mgr.transition_to(StateId::Recording, "rec_err");
    mgr.transition_to(StateId::Sending, "send_err");
    mgr.transition_to(StateId::Done, "done_err");
    std::strcpy(mgr.context().result.transcript, "вопрос");
    std::strcpy(mgr.context().result.error, "500 Internal Server Error");
    mgr.context().followup_streak = 2;
    mgr.step(nullptr, 0);
    TEST_ASSERT(mgr.current_id() == StateId::Idle, "Result with error exits to IDLE");
    TEST_ASSERT(mgr.context().followup_streak == 0, "Streak reset on error");

    std::cout << "  [PASS] test_t117_done_empty_or_error_rejects_followup\n";
}

void test_t117_idle_settle_guard() {
    IdleState idle;
    VoiceContext ctx{};
    idle.on_enter(ctx, "boot");
    TEST_ASSERT(idle.ignore_audio_until_ms() == IDLE_SETTLE_GUARD_MS, "Idle settle guard set to 500 ms");
    TEST_ASSERT(ctx.followup_streak == 0, "Idle::on_enter resets followup_streak to 0");

    std::cout << "  [PASS] test_t117_idle_settle_guard\n";
}

int main() {
    std::cout << "=== T111/T116/T117 Hostcheck: C++ Voice FSM Suite ===\n";
    test_initialization();
    test_guard_conditions();
    test_interruption_lifecycle();
    test_vad_silence_3000ms();
    test_button_vad_silence_3000ms();
    test_done_speech_interruption();
    test_response_parser();
    test_context_resets_and_dialogue_cleanup();
    test_followup_speech_streak_guard();
    test_history_version_monotonic();
    test_t116_fsm_improvements();
    test_t117_vad_filter_speech_vs_noise();
    test_t117_followup_noise_rejection();
    test_t117_recording_followup_abort_on_noise();
    test_t117_done_streak_limit();
    test_t117_done_empty_or_error_rejects_followup();
    test_t117_idle_settle_guard();
    std::cout << "===========================================\n";
    std::cout << "ALL " << g_tests_passed << "/" << g_tests_run << " CHECKS PASSED!\n";
    return 0;
}
