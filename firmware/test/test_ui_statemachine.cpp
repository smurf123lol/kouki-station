#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <thread>

#include "app_audio_out.h"
#include "app_config.h"
#include "app_history.h"
#include "app_ota.h"
#include "app_speech.h"
#include "app_time.h"
#include "app_voice.h"
#include "app_wifi.h"
#include "esp_app_desc.h"
#include "esp_board_init.h"
#include "ui/ui_common.hpp"
#include "ui/ui_state_manager.hpp"
#include "ui/widgets/ui_card_widget.hpp"
#include "ui/widgets/ui_orb_widget.hpp"

// --------------------------------------------------------------------------
// Контролируемая подмена operator new(std::nothrow) для реальной проверки OOM
// в HistoryTile::refresh() и CardWidget::set_result().
// --------------------------------------------------------------------------
static bool g_fail_nothrow_alloc = false;

void *operator new(std::size_t sz)
{
    void *p = std::malloc(sz == 0 ? 1 : sz);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void *operator new(std::size_t sz, const std::nothrow_t &) noexcept
{
    if (g_fail_nothrow_alloc) {
        return nullptr;
    }
    return std::malloc(sz == 0 ? 1 : sz);
}

void *operator new[](std::size_t sz)
{
    void *p = std::malloc(sz == 0 ? 1 : sz);
    if (!p) {
        throw std::bad_alloc();
    }
    return p;
}

void *operator new[](std::size_t sz, const std::nothrow_t &) noexcept
{
    if (g_fail_nothrow_alloc) {
        return nullptr;
    }
    return std::malloc(sz == 0 ? 1 : sz);
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

// Шрифты, объявленные в ui_common.hpp
const lv_font_t font_ru_14{14};
const lv_font_t font_ru_16{16};
const lv_font_t font_ru_20{20};

// --------------------------------------------------------------------------
// Управляемые С-заглушки зависимостей UI
// --------------------------------------------------------------------------
static kouki_voice_state_t g_voice_state = KOUKI_VOICE_IDLE;
static kouki_voice_result_t g_voice_result{};
static kouki_voice_metrics_t g_voice_metrics{};
static int g_start_rec_calls = 0;
static int g_stop_send_calls = 0;

static bool g_speech_playing = false;
static bool g_speech_busy = false;
static int g_speech_abort_calls = 0;

static int g_audio_lock_depth = 0;
static int g_audio_lock_calls = 0;
static int g_audio_unlock_calls = 0;
static int g_play_vol = 80;
static int g_get_vol_calls = 0;
static int g_get_vol_while_locked = 0;
static int g_set_vol_calls = 0;
static int g_set_vol_while_locked = 0;

static bool g_wifi_connected = false;
static uint32_t g_wifi_seq = 0;
static int8_t g_wifi_rssi = -45;
static const char *g_wifi_ip = "10.0.2.15";
static int g_wifi_info_calls = 0;

static bool g_cfg_silent = false;
static bool g_cfg_wake_en = true;
static uint8_t g_cfg_wake_th = 75;
static int g_cfg_save_calls = 0;
static int g_ota_check_calls = 0;

extern "C" {

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    default: return "ESP_FAIL";
    }
}

esp_err_t kouki_audio_out_init(void) { return ESP_OK; }

void kouki_audio_out_lock(void)
{
    g_audio_lock_depth++;
    g_audio_lock_calls++;
}

void kouki_audio_out_unlock(void)
{
    g_audio_lock_depth--;
    g_audio_unlock_calls++;
}

esp_err_t esp_audio_get_play_vol(int *volume)
{
    g_get_vol_calls++;
    if (g_audio_lock_depth == 1) {
        g_get_vol_while_locked++;
    }
    if (volume) *volume = g_play_vol;
    return ESP_OK;
}

esp_err_t esp_audio_set_play_vol(int volume)
{
    g_set_vol_calls++;
    if (g_audio_lock_depth == 1) {
        g_set_vol_while_locked++;
    }
    g_play_vol = volume;
    return ESP_OK;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc{"0.1.5"};
    return &desc;
}

static bool g_snapshot_return_value = true;
static int g_dismiss_error_calls = 0;

bool kouki_voice_get_snapshot(kouki_voice_state_t *state, kouki_voice_result_t *result)
{
    if (!g_snapshot_return_value) {
        return false;
    }
    if (state) *state = g_voice_state;
    if (result) *result = g_voice_result;
    return true;
}

kouki_voice_state_t kouki_voice_get_state(void)
{
    return g_voice_state;
}

esp_err_t kouki_voice_dismiss_error(void)
{
    g_dismiss_error_calls++;
    if (g_voice_state == KOUKI_VOICE_ERROR) {
        g_voice_state = KOUKI_VOICE_IDLE;
    }
    return ESP_OK;
}

void kouki_voice_get_metrics(kouki_voice_metrics_t *metrics)
{
    if (metrics) *metrics = g_voice_metrics;
}

bool kouki_voice_is_recording(void)
{
    return g_voice_state == KOUKI_VOICE_RECORDING;
}

esp_err_t kouki_voice_start_recording(void)
{
    g_start_rec_calls++;
    return ESP_OK;
}

esp_err_t kouki_voice_stop_and_send(void)
{
    g_stop_send_calls++;
    return ESP_OK;
}

bool kouki_speech_is_playing(void) { return g_speech_playing; }
bool kouki_speech_busy(void) { return g_speech_busy; }
void kouki_speech_abort(void)
{
    g_speech_abort_calls++;
    g_speech_playing = false;
    g_speech_busy = false;
}

bool kouki_wifi_is_connected(void) { return g_wifi_connected; }
uint32_t kouki_wifi_state_seq(void) { return g_wifi_seq; }

esp_err_t kouki_wifi_get_info(char *ip_buf, size_t ip_len, int8_t *rssi)
{
    g_wifi_info_calls++;
    if (ip_buf && ip_len > 0) {
        std::snprintf(ip_buf, ip_len, "%s", g_wifi_ip ? g_wifi_ip : "");
    }
    if (rssi) {
        *rssi = g_wifi_rssi;
    }
    return ESP_OK;
}

const char *kouki_config_get_wifi_ssid(void) { return "kouki-sim"; }
bool kouki_config_get_wake_enabled(void) { return g_cfg_wake_en; }
esp_err_t kouki_config_set_wake_enabled(bool enabled) { g_cfg_wake_en = enabled; return ESP_OK; }
uint8_t kouki_config_get_wake_threshold(void) { return g_cfg_wake_th; }
esp_err_t kouki_config_set_wake_threshold(uint8_t th) { g_cfg_wake_th = th; return ESP_OK; }
bool kouki_config_is_silent(void) { return g_cfg_silent; }
void kouki_config_set_silent(bool silent) { g_cfg_silent = silent; }
esp_err_t kouki_config_save(void) { g_cfg_save_calls++; return ESP_OK; }

void kouki_ota_check_now(void) { g_ota_check_calls++; }

} // extern "C"

// --------------------------------------------------------------------------
// Test helpers
// --------------------------------------------------------------------------
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
} while (0)

using namespace kouki::ui;

static void simulate_touch(lv_obj_t *target, int x0, int y0, int x1, int y1, lv_obj_t *scroll_obj = nullptr)
{
    lv_indev_t indev{};
    indev.point = {static_cast<lv_coord_t>(x0), static_cast<lv_coord_t>(y0)};
    indev.scroll_obj = nullptr;
    lv_mock_send_event(target, LV_EVENT_PRESSED, &indev);

    indev.point = {static_cast<lv_coord_t>(x1), static_cast<lv_coord_t>(y1)};
    indev.scroll_obj = scroll_obj;
    lv_mock_send_event(target, LV_EVENT_PRESSING, &indev);

    lv_mock_send_event(target, LV_EVENT_CLICKED, &indev);
}

static void reset_voice_to_idle()
{
    g_voice_state = KOUKI_VOICE_IDLE;
    g_voice_result = kouki_voice_result_t{};
    g_voice_metrics = kouki_voice_metrics_t{};
    UiStateManager::instance().step(33);
}

static void test_pure_helpers()
{
    std::cout << "[TEST] 1. Pure UI geometry, VAD gap math, and UTF-8 trim helpers...\n";
    TEST_ASSERT(VAD_ARC_GAP_MS == 800, "VAD_ARC_GAP_MS == 800");
    TEST_ASSERT(WIFI_RSSI_POLL_MS == 10000, "WIFI_RSSI_POLL_MS == 10000");
    TEST_ASSERT(TILE_INACTIVITY_MS == 20000, "TILE_INACTIVITY_MS == 20000");
    TEST_ASSERT(MINI_ORB_TAP_CENTER_Y_OFFSET == -138, "MINI_ORB_TAP_CENTER_Y_OFFSET == -138");
    TEST_ASSERT(compute_vad_arc_gap_ms(3000) == 800, "gap(3000) == 800");
    TEST_ASSERT(compute_vad_arc_gap_ms(1500) == 750, "gap(1500) == 750");

    char str[32];
    std::strcpy(str, "Привет");
    str[3] = '\0';
    utf8_trim_tail(str);
    TEST_ASSERT(std::strcmp(str, "П") == 0, "utf8_trim_tail strips incomplete lead byte");
}

static void test_init_and_ota_modal_guard()
{
    std::cout << "[TEST] 2. Real UiStateManager::init & OTA modal blocking/clear...\n";
    kouki_history_init();
    kouki_history_clear();
    kouki_time_init("MSK-3");

    UiStateManager &mgr = UiStateManager::instance();
    mgr.init();
    mgr.step(33);

    UiContext &ctx = mgr.get_context();
    TEST_ASSERT(ctx.tileview != nullptr && ctx.tile_voice != nullptr, "Real LVGL tile tree built");
    TEST_ASSERT(mgr.get_active_tile() == TileId::TILE_VOICE, "Starts on TILE_VOICE");
    TEST_ASSERT(lv_obj_has_flag(ctx.tileview, LV_OBJ_FLAG_SCROLLABLE), "TileView scroll initially unlocked");
    TEST_ASSERT(lv_obj_has_flag(ctx.response_card->get_root(), LV_OBJ_FLAG_HIDDEN), "Card initially hidden");

    lv_obj_t *orb_root = ctx.orb->get_root();
    TEST_ASSERT(orb_root->children.size() == 8, "OrbWidget created expected 8 child widgets");
    lv_obj_t *wifi_dot = orb_root->children[0];
    lv_obj_t *silence_arc = orb_root->children[2];
    lv_obj_t *time_lbl = orb_root->children[4];
    TEST_ASSERT(wifi_dot->bg_color == COLOR_DANGER, "Wi-Fi dot starts red when disconnected");
    TEST_ASSERT(lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "VAD arc initially hidden");
    TEST_ASSERT(!lv_obj_has_flag(time_lbl, LV_OBJ_FLAG_HIDDEN), "Clock visible in Idle");

    mgr.show_ota_progress("OTA 42%");
    mgr.step(33);
    const int before_rec = g_start_rec_calls;
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_start_rec_calls == before_rec, "OTA modal blocks recording tap");

    mgr.clear_ota();
    mgr.step(33);
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_start_rec_calls == before_rec + 1, "clear_ota re-enables recording tap");
    g_start_rec_calls = 0;
}

static void test_real_touch_zones_swipe_and_press_cancel()
{
    std::cout << "[TEST] 3. Real UiStateManager::on_tile_voice_event zones, drag, scroll, press-loss & state-change reset...\n";
    UiStateManager &mgr = UiStateManager::instance();
    UiContext &ctx = mgr.get_context();
    g_start_rec_calls = 0;

    // Outside tap zone (bezel left rim)
    simulate_touch(ctx.tile_voice, 25, 180, 25, 180);
    TEST_ASSERT(g_start_rec_calls == 0, "Bezel tap (25,180) rejected");

    // Swipe >14px starting from center
    simulate_touch(ctx.tile_voice, 180, 180, 165, 180);
    TEST_ASSERT(g_start_rec_calls == 0, "Horizontal drag 15px from center rejected as swipe");

    // Active LVGL scroll object
    simulate_touch(ctx.tile_voice, 180, 180, 182, 180, ctx.tileview);
    TEST_ASSERT(g_start_rec_calls == 0, "Press while TileView scrolling rejected");

    // PRESS_LOST cancels press
    lv_indev_t indev{};
    indev.point = {180, 180};
    lv_mock_send_event(ctx.tile_voice, LV_EVENT_PRESSED, &indev);
    lv_mock_send_event(ctx.tile_voice, LV_EVENT_PRESS_LOST, &indev);
    lv_mock_send_event(ctx.tile_voice, LV_EVENT_CLICKED, &indev);
    TEST_ASSERT(g_start_rec_calls == 0, "PRESS_LOST cancels tap");

    // transition_to() resets m_press_active
    lv_mock_send_event(ctx.tile_voice, LV_EVENT_PRESSED, &indev);
    mgr.transition_to(UiVoiceSubStateId::SUB_SENDING);
    mgr.transition_to(UiVoiceSubStateId::SUB_IDLE);
    lv_mock_send_event(ctx.tile_voice, LV_EVENT_CLICKED, &indev);
    TEST_ASSERT(g_start_rec_calls == 0, "transition_to clears stale press state");

    // Clean center tap with 7px jitter (<= 14px) succeeds
    simulate_touch(ctx.tile_voice, 180, 180, 185, 185);
    TEST_ASSERT(g_start_rec_calls == 1, "Clean center tap triggers kouki_voice_start_recording()");
}

static void test_real_orb_recording_vad_gap_and_interpolation()
{
    std::cout << "[TEST] 4. Real SubRecording focus override, scroll lock, 800ms VAD gap & 96ms interp clamp...\n";
    UiStateManager &mgr = UiStateManager::instance();
    UiContext &ctx = mgr.get_context();

    lv_obj_set_tile_id(ctx.tileview, 2, 0, LV_ANIM_OFF);
    TEST_ASSERT(mgr.get_active_tile() == TileId::TILE_SETTINGS, "Switched to Settings tile");

    g_voice_state = KOUKI_VOICE_RECORDING;
    g_voice_metrics = kouki_voice_metrics_t{true, false, 100, 0, 3000};
    mgr.step(33);

    TEST_ASSERT(mgr.get_active_tile() == TileId::TILE_VOICE, "Recording forces instant focus back to TILE_VOICE");
    TEST_ASSERT(!lv_obj_has_flag(ctx.tileview, LV_OBJ_FLAG_SCROLLABLE), "TileView scroll locked in SubRecording");

    lv_obj_t *silence_arc = ctx.orb->get_root()->children[2];
    TEST_ASSERT(lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Arc hidden before speech");

    // Inside 800ms grace gap -> hidden
    g_voice_metrics = kouki_voice_metrics_t{true, true, 600, 500, 3000};
    mgr.step(33);
    TEST_ASSERT(lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Arc hidden at 500ms (<800ms gap)");

    // Interpolation across 33ms ticks: Step 1 snaps metric 500->750, Step 2 advances 750->783 (hidden), Step 3 -> 816 (>=800 -> visible)
    g_voice_metrics = kouki_voice_metrics_t{true, true, 850, 750, 3000};
    mgr.step(33);
    TEST_ASSERT(lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Arc hidden at snapped 750ms");
    mgr.step(33);
    TEST_ASSERT(lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Arc still hidden at interpolated 783ms");
    mgr.step(33);
    TEST_ASSERT(!lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Arc appears smoothly once interpolated >= 800ms");
    const uint16_t angle_at_816 = silence_arc->arc_end;
    TEST_ASSERT(angle_at_816 >= 355 && angle_at_816 <= 360, "Initial visible angle starts near 360 deg");

    // Advance without metric update -> clamped at 750 + 96 = 846ms
    for (int i = 0; i < 5; i++) {
        mgr.step(33);
    }
    TEST_ASSERT(silence_arc->arc_end >= 350, "Interpolation clamped to <= 96ms ahead of metric step");

    // Speech resumes -> immediate hide
    g_voice_metrics = kouki_voice_metrics_t{true, true, 1200, 0, 3000};
    mgr.step(33);
    TEST_ASSERT(lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Arc hides immediately when speech resumes");

    // Follow-up limit (1500ms, clamped gap 750ms)
    g_voice_metrics = kouki_voice_metrics_t{true, true, 1400, 1125, 1500};
    mgr.step(33);
    TEST_ASSERT(!lv_obj_has_flag(silence_arc, LV_OBJ_FLAG_HIDDEN), "Followup arc visible at 1125ms");
    TEST_ASSERT(silence_arc->arc_end == 180, "Followup arc angle == 180 at midpoint");

    reset_voice_to_idle();
    TEST_ASSERT(lv_obj_has_flag(ctx.tileview, LV_OBJ_FLAG_SCROLLABLE), "Scroll unlocked upon return to Idle");
}

static void test_real_done_card_miniorb_speech_abort_and_oom()
{
    std::cout << "[TEST] 5. Real SubDone mini-orb vs card tap, speech abort (playing/busy), scroll & OOM fallback...\n";
    UiStateManager &mgr = UiStateManager::instance();
    UiContext &ctx = mgr.get_context();

    lv_obj_t *card_root = ctx.response_card->get_root();
    lv_obj_t *answer_lbl = card_root->children[2];

    // CardWidget OOM fallback -> command
    g_fail_nothrow_alloc = true;
    g_voice_result = kouki_voice_result_t{};
    std::strcpy(g_voice_result.outcome, "executed");
    std::strcpy(g_voice_result.command, "list_tasks");
    g_voice_state = KOUKI_VOICE_DONE;
    mgr.step(33);
    TEST_ASSERT(!lv_obj_has_flag(card_root, LV_OBJ_FLAG_HIDDEN), "Card visible under OOM");
    TEST_ASSERT(answer_lbl->text == "list_tasks", "Card OOM fallback displays command");

    // CardWidget OOM fallback -> intent
    reset_voice_to_idle();
    g_voice_result = kouki_voice_result_t{};
    std::strcpy(g_voice_result.intent, "ask");
    g_voice_state = KOUKI_VOICE_DONE;
    mgr.step(33);
    TEST_ASSERT(answer_lbl->text == "ask", "Card OOM fallback displays intent");
    g_fail_nothrow_alloc = false;
    reset_voice_to_idle();

    // Real SubDone long answer auto-scroll + LV_EVENT_PRESSED stops auto-scroll + mini-orb tap
    g_voice_result = kouki_voice_result_t{};
    g_voice_result.has_transcript = true;
    std::strcpy(g_voice_result.transcript, "Подробный статус");
    std::strcpy(g_voice_result.intent, "ask");
    std::strcpy(g_voice_result.outcome, "executed");
    std::strcpy(g_voice_result.answer,
                "Длинный контрольный ответ ассистента для реальной проверки автоскролла карточки в SubDoneState "
                "и остановки автоскролла по LV_EVENT_PRESSED, а также проверки разделения зоны карточки и мини-сферы.");
    card_root->scroll_bottom = 250;
    g_voice_state = KOUKI_VOICE_DONE;
    mgr.step(33);

    mgr.step(1000);
    const lv_coord_t y_after_auto = card_root->scroll_y;
    TEST_ASSERT(y_after_auto >= 19, "Long answer auto-scrolls card (~20px/s)");

    lv_mock_send_event(card_root, LV_EVENT_PRESSED);
    mgr.step(1000);
    TEST_ASSERT(card_root->scroll_y == y_after_auto, "LV_EVENT_PRESSED stops card auto-scroll");

    // Card center tap must NOT start recording or abort speech
    g_speech_busy = true;
    g_speech_playing = false;
    g_speech_abort_calls = 0;
    g_start_rec_calls = 0;
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_speech_abort_calls == 0 && g_start_rec_calls == 0,
                "Card center tap in SubDone ignored by voice tile handler");

    // Mini-orb top tap (180, 42) aborts busy speech and starts recording
    simulate_touch(ctx.tile_voice, 180, 42, 180, 42);
    TEST_ASSERT(g_speech_abort_calls == 1, "Mini-orb tap aborts speech even when only busy()");
    TEST_ASSERT(g_start_rec_calls == 1, "Mini-orb tap starts new recording");

    lv_obj_t *transcript_lbl = card_root->children[1];
    TEST_ASSERT(transcript_lbl->h == LV_SIZE_CONTENT && answer_lbl->h == LV_SIZE_CONTENT,
                "Card labels use LV_SIZE_CONTENT height inside flex column (no overlap)");

    // Also test abort when playing() == true
    g_speech_playing = true;
    g_speech_busy = false;
    mgr.toggle_recording();
    TEST_ASSERT(g_speech_abort_calls == 2, "toggle_recording aborts playing() speech");

    // In SENDING: toggle_recording MUST refuse without aborting speech or calling start_recording
    const int abort_before_send = g_speech_abort_calls;
    const int start_before_send = g_start_rec_calls;
    g_voice_state = KOUKI_VOICE_SENDING;
    mgr.toggle_recording();
    TEST_ASSERT(g_speech_abort_calls == abort_before_send && g_start_rec_calls == start_before_send,
                "toggle_recording refuses cleanly in SENDING without abort or start");

    // In DONE before speech worker marks playing/busy: toggle_recording still calls abort() unconditionally
    g_speech_playing = false;
    g_speech_busy = false;
    g_voice_state = KOUKI_VOICE_DONE;
    mgr.toggle_recording();
    TEST_ASSERT(g_speech_abort_calls == abort_before_send + 1 && g_start_rec_calls == start_before_send + 1,
                "toggle_recording aborts pre-worker DONE speech unconditionally before start");

    reset_voice_to_idle();
}

static void test_real_history_tile_oom_retry_and_autoreturn()
{
    std::cout << "[TEST] 6. Real HistoryTile OOM retry preserves version + 20s auto-return...\n";
    UiStateManager &mgr = UiStateManager::instance();
    UiContext &ctx = mgr.get_context();

    lv_obj_t *hist_root = ctx.tile_history->children[0];
    lv_obj_t *list_cont = hist_root->children[1];
    lv_obj_t *empty_lbl = hist_root->children[2];
    TEST_ASSERT(list_cont->children.empty() && !lv_obj_has_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN),
                "History initially empty");

    for (int i = 0; i < KOUKI_HISTORY_CAPACITY; i++) {
        kouki_voice_result_t r{};
        r.has_transcript = true;
        std::snprintf(r.transcript, sizeof(r.transcript), "Запрос #%d", i + 1);
        std::snprintf(r.intent, sizeof(r.intent), (i % 2 == 0) ? "add_task" : "ask");
        std::snprintf(r.outcome, sizeof(r.outcome), (i % 2 == 0) ? "created" : "executed");
        if (i % 2 == 0) {
            std::snprintf(r.task_title, sizeof(r.task_title), "Задача #%d", i + 1);
        } else {
            std::snprintf(r.answer, sizeof(r.answer), "Длинный кириллический ответ #%d для истории.", i + 1);
        }
        kouki_history_push(&r);
    }

    // Force OOM inside HistoryTile::refresh()
    g_fail_nothrow_alloc = true;
    mgr.step(33);
    TEST_ASSERT(list_cont->children.empty(), "OOM leaves list intact without crash");
    TEST_ASSERT(!lv_obj_has_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN), "OOM did not corrupt empty placeholder");

    // Clear OOM -> next step MUST retry because m_last_history_version was NOT updated
    g_fail_nothrow_alloc = false;
    mgr.step(33);
    TEST_ASSERT(list_cont->children.size() == KOUKI_HISTORY_CAPACITY,
                "HistoryTile retried automatically after OOM and rendered all 10 cards");
    TEST_ASSERT(lv_obj_has_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN), "Empty label hidden after successful retry");

    // Auto-return 20s + touch reset
    lv_obj_set_tile_id(ctx.tileview, 0, 0, LV_ANIM_OFF);
    mgr.step(10000);
    lv_mock_send_event(list_cont, LV_EVENT_PRESSED);
    mgr.step(19900);
    TEST_ASSERT(mgr.get_active_tile() == TileId::TILE_HISTORY, "Touch reset keeps History active up to 19.9s");
    mgr.step(200);
    TEST_ASSERT(mgr.get_active_tile() == TileId::TILE_VOICE, "Auto-returns from History after 20s inactivity");
}

static void test_real_settings_tile_wifi_volume_mutex_and_setup_modal()
{
    std::cout << "[TEST] 7. Real SettingsTile reactive Wi-Fi, 10s poll, volume audio_out lock & Setup modal...\n";
    UiStateManager &mgr = UiStateManager::instance();
    UiContext &ctx = mgr.get_context();

    // Wi-Fi reactive update on OrbWidget & SettingsTile
    g_wifi_info_calls = 0;
    mgr.step(5000);
    TEST_ASSERT(g_wifi_info_calls == 0, "No Settings Wi-Fi poll at 5s");

    g_wifi_connected = true;
    g_wifi_seq++;
    mgr.step(33);
    lv_obj_t *wifi_dot = ctx.orb->get_root()->children[0];
    TEST_ASSERT(wifi_dot->bg_color == COLOR_OK, "Orb Wi-Fi dot turns green immediately on seq bump");
    TEST_ASSERT(g_wifi_info_calls == 1, "Settings Wi-Fi refreshed immediately on seq bump");

    mgr.step(9000);
    TEST_ASSERT(g_wifi_info_calls == 1, "No Settings Wi-Fi poll at 9s");
    mgr.step(1000);
    TEST_ASSERT(g_wifi_info_calls == 2, "Fallback Settings Wi-Fi poll triggers at 10s");

    // Switch to Settings tile -> immediate refresh on tile entry
    lv_obj_set_tile_id(ctx.tileview, 2, 0, LV_ANIM_OFF);
    mgr.step(33);
    TEST_ASSERT(g_wifi_info_calls == 3, "Immediate refresh when entering Settings tile");

    lv_obj_t *settings_root = ctx.tile_settings->children[0];
    lv_obj_t *scroll_cont = settings_root->children[1];
    lv_obj_t *vol_lbl = scroll_cont->children[0];
    lv_obj_t *vol_slider = scroll_cont->children[1];
    lv_obj_t *wake_sw = scroll_cont->children[2]->children[1];
    lv_obj_t *silent_sw = scroll_cont->children[6]->children[1]; /* T119: +1 подсказка под слайдером порога */
    lv_obj_t *ota_btn = scroll_cont->children[9]; /* T119: +1 подсказка под слайдером порога */

    // Initial get_play_vol in SettingsTile ctor held audio_out lock cleanly
    TEST_ASSERT(g_get_vol_calls >= 1 && g_get_vol_while_locked == g_get_vol_calls,
                "Initial esp_audio_get_play_vol executed inside kouki_audio_out_lock()");

    // Volume slider holds audio_out mutex around esp_audio_set_play_vol()
    g_audio_lock_depth = 0;
    g_audio_lock_calls = 0;
    g_audio_unlock_calls = 0;
    g_set_vol_calls = 0;
    g_set_vol_while_locked = 0;
    lv_slider_set_value(vol_slider, 55, LV_ANIM_OFF);
    lv_mock_send_event(vol_slider, LV_EVENT_VALUE_CHANGED);
    TEST_ASSERT(g_set_vol_calls == 1 && g_set_vol_while_locked == 1,
                "esp_audio_set_play_vol executed inside kouki_audio_out_lock()");
    TEST_ASSERT(g_audio_lock_depth == 0 && g_audio_lock_calls == 1 && g_audio_unlock_calls == 1,
                "kouki_audio_out_lock/unlock balanced");
    TEST_ASSERT(vol_lbl->text == "Громкость: 55%", "Volume label updated");

    // Wake slider: VALUE_CHANGED updates RAM, RELEASED commits to NVS
    lv_obj_t *wake_slider = scroll_cont->children[4];
    const int cfg_save_before_slider = g_cfg_save_calls;
    lv_slider_set_value(wake_slider, 70, LV_ANIM_OFF);
    lv_mock_send_event(wake_slider, LV_EVENT_VALUE_CHANGED);
    TEST_ASSERT(kouki_config_get_wake_threshold() == 70, "Wake sensitivity RAM updated on VALUE_CHANGED");
    TEST_ASSERT(g_cfg_save_calls == cfg_save_before_slider, "Config NOT saved on VALUE_CHANGED");
    lv_mock_send_event(wake_slider, LV_EVENT_RELEASED);
    TEST_ASSERT(g_cfg_save_calls == cfg_save_before_slider + 1, "Config saved to NVS on RELEASED");

    // Switches and OTA button
    lv_obj_clear_state(wake_sw, LV_STATE_CHECKED);
    lv_mock_send_event(wake_sw, LV_EVENT_VALUE_CHANGED);
    TEST_ASSERT(g_cfg_wake_en == false && g_cfg_save_calls >= 1, "Wake switch saves config");

    lv_obj_add_state(silent_sw, LV_STATE_CHECKED);
    lv_mock_send_event(silent_sw, LV_EVENT_VALUE_CHANGED);
    TEST_ASSERT(g_cfg_silent == true, "Silent switch updates config");

    const int ota_before = g_ota_check_calls;
    lv_mock_send_event(ota_btn, LV_EVENT_CLICKED);
    TEST_ASSERT(g_ota_check_calls == ota_before + 1, "OTA button calls kouki_ota_check_now()");

    // 20s auto-return from Settings
    mgr.step(20000);
    TEST_ASSERT(mgr.get_active_tile() == TileId::TILE_VOICE, "Auto-returns from Settings after 20s");

    // Final modal guard check: Setup blocks tap
    mgr.show_setup("Kouki-Setup-ABCD", "http://192.168.4.1");
    const int before_rec = g_start_rec_calls;
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_start_rec_calls == before_rec, "Setup modal blocks recording tap");
    mgr.clear_setup();
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_start_rec_calls == before_rec + 1, "clear_setup unblocks recording tap");
    g_start_rec_calls = 0;
}


static void test_t116_ui_fixes()
{
    std::cout << "[TEST] 8. T116 UI Fixes: Cross-thread OTA, snapshot fallback & SubError dismiss...\n";
    UiStateManager &mgr = UiStateManager::instance();
    UiContext &ctx = mgr.get_context();

    // 1. Cross-thread show_ota_progress & clear_ota
    std::thread t1([&]() {
        mgr.show_ota_progress("Thread OTA 50%");
    });
    t1.join();

    const int before_rec = g_start_rec_calls;
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_start_rec_calls == before_rec, "Cross-thread OTA blocks touch before step");

    mgr.step(33);

    std::thread t2([&]() {
        mgr.clear_ota();
    });
    t2.join();

    mgr.step(33);
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_start_rec_calls == before_rec + 1, "Touch re-enabled after clear_ota drained");
    g_start_rec_calls = 0;

    // 2. Snapshot timeout fallback to kouki_voice_get_state()
    g_snapshot_return_value = false;
    g_voice_state = KOUKI_VOICE_ERROR;
    g_voice_result = kouki_voice_result_t{};
    std::snprintf(g_voice_result.error, sizeof(g_voice_result.error), "Mock error");

    mgr.step(33);
    TEST_ASSERT(mgr.get_current_substate_id() == UiVoiceSubStateId::SUB_ERROR,
                "UiStateManager falls back to kouki_voice_get_state on snapshot false");

    // 3. SubErrorState on_tap dismisses error
    g_dismiss_error_calls = 0;
    simulate_touch(ctx.tile_voice, 180, 180, 180, 180);
    TEST_ASSERT(g_dismiss_error_calls == 1, "Tapping in SubErrorState calls kouki_voice_dismiss_error");
    TEST_ASSERT(mgr.get_current_substate_id() == UiVoiceSubStateId::SUB_IDLE, "Returned to SUB_IDLE immediately");

    // Clean up
    g_snapshot_return_value = true;
    g_voice_state = KOUKI_VOICE_IDLE;
    mgr.step(33);
}

int main()
{
    std::cout << "========================================================\n";
    std::cout << "  Kouki Station T116 Real UI C++ Host Check Suite\n";
    std::cout << "========================================================\n";

    test_pure_helpers();
    test_init_and_ota_modal_guard();
    test_real_touch_zones_swipe_and_press_cancel();
    test_real_orb_recording_vad_gap_and_interpolation();
    test_real_done_card_miniorb_speech_abort_and_oom();
    test_real_history_tile_oom_retry_and_autoreturn();
    test_real_settings_tile_wifi_volume_mutex_and_setup_modal();
    test_t116_ui_fixes();

    std::cout << "========================================================\n";
    std::cout << "  RESULTS: " << g_tests_passed << "/" << g_tests_run << " tests passed (0 failures)\n";
    std::cout << "========================================================\n";
    return 0;
}
