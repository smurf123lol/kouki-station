#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "app_voice.h"
#include "app_wifi.h"
#include "esp_log.h"
#include "lvgl.h"

/*
 * Шрифты с кириллицей: во встроенных montserrat её нет, поэтому 14/16/20
 * собраны lv_font_conv из Arial (см. main/ui и docs/build.md).
 */
LV_FONT_DECLARE(font_ru_14);
LV_FONT_DECLARE(font_ru_16);
LV_FONT_DECLARE(font_ru_20);

static const char *TAG = "kouki.ui";

/* Палитра та же, что у дашборда: бумага, чернила, вермильон. */
#define COLOR_INK      0x0B0A08
#define COLOR_PAPER    0x16130F
#define COLOR_SAND     0xE2D2B2
#define COLOR_DIM      0x8B7D66
#define COLOR_ACCENT   0xD8552F
#define COLOR_ASK      0x2F6B6B
#define COLOR_OK       0x38D483
#define COLOR_WARN     0xFFCC33
#define COLOR_DANGER   0xFF4D6D

static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_mode_button;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_status;
static lv_obj_t *s_answer;

/*
 * Режим выбирается пилюлей, запись начинается одной большой кнопкой.
 * Две «стартовые» кнопки путали: нажатие не той создавало задачу вместо ответа.
 */
static kouki_session_t s_mode = KOUKI_SESSION_TASK;
static kouki_session_t s_recording_mode = KOUKI_SESSION_TASK;

static void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

/* Пилюля режима и цвет главной кнопки в покое. */
static void apply_mode(void)
{
    const bool task = (s_mode == KOUKI_SESSION_TASK);

    lv_label_set_text(s_mode_label, task ? "Режим: задача" : "Режим: вопрос");
    lv_obj_set_style_border_color(s_mode_button, lv_color_hex(task ? COLOR_DIM : COLOR_ASK), 0);
    lv_obj_set_style_bg_color(s_mode_button, lv_color_hex(task ? COLOR_PAPER : COLOR_ASK), 0);

    if (!kouki_voice_is_recording()) {
        lv_label_set_text(s_button_label, task ? "Записать" : "Спросить");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(task ? COLOR_ACCENT : COLOR_ASK), 0);
    }
}

static void on_mode_clicked(lv_event_t *event)
{
    (void)event;

    if (kouki_voice_is_recording()) {
        return;     /* пока пишем, смысл записи уже определён */
    }

    s_mode = (s_mode == KOUKI_SESSION_TASK) ? KOUKI_SESSION_ASK : KOUKI_SESSION_TASK;
    apply_mode();
    ESP_LOGI(TAG, "режим: %s", s_mode == KOUKI_SESSION_TASK ? "task" : "ask");
}

static void on_main_clicked(lv_event_t *event)
{
    (void)event;

    if (kouki_voice_is_recording()) {
        ESP_LOGI(TAG, "останавливаю запись");
        kouki_voice_stop_and_send();
        return;
    }

    if (kouki_voice_set_session(s_mode) != ESP_OK) {
        ESP_LOGW(TAG, "сессию сменить нельзя: идёт запись");
        return;
    }

    s_recording_mode = s_mode;
    ESP_LOGI(TAG, "начинаю запись: %s", s_mode == KOUKI_SESSION_TASK ? "task" : "ask");
    kouki_voice_start_recording();
}

/* Перерисовывает экран по текущему состоянию записи/отправки. */
static void ui_tick(lv_timer_t *timer)
{
    (void)timer;

    kouki_voice_state_t state = KOUKI_VOICE_IDLE;
    kouki_voice_result_t result = {0};
    kouki_voice_get_snapshot(&state, &result);

    switch (state) {
    case KOUKI_VOICE_IDLE:
        set_status(kouki_wifi_is_connected() ? "Нажмите и говорите" : "Нет сети", COLOR_DIM);
        lv_obj_clear_state(s_mode_button, LV_STATE_DISABLED);
        apply_mode();
        break;

    case KOUKI_VOICE_RECORDING:
        set_status(s_recording_mode == KOUKI_SESSION_TASK ? "Слушаю… запишу задачей"
                                                          : "Слушаю… разберу как вопрос",
                   COLOR_WARN);
        lv_label_set_text(s_button_label, "Отправить");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_WARN), 0);
        /* Пока пишем, режим не меняется. */
        lv_obj_add_state(s_mode_button, LV_STATE_DISABLED);
        break;

    case KOUKI_VOICE_SENDING:
        set_status("Думаю…", COLOR_DIM);
        lv_label_set_text(s_button_label, "…");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_PAPER), 0);
        lv_obj_add_state(s_mode_button, LV_STATE_DISABLED);
        break;

    case KOUKI_VOICE_DONE:
    case KOUKI_VOICE_ERROR: {
        char text[256] = {0};
        size_t used = 0;

        if (result.error[0] != '\0') {
            set_status("Не вышло", COLOR_DANGER);
            snprintf(text, sizeof(text), "%s", result.error);
            lv_label_set_text(s_answer, text);
        } else {
            const char *outcome = result.outcome[0] ? result.outcome : "executed";
            const bool has_answer = result.answer[0] != '\0';
            set_status(has_answer                          ? "Ответ"
                       : strcmp(outcome, "created") == 0   ? "Задача записана"
                       : strcmp(outcome, "executed") == 0  ? "Готово"
                       : "На разбор", COLOR_OK);

            if (result.has_transcript && result.transcript[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "«%s»\n", result.transcript);
            }
            if (has_answer) {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", result.answer);
            } else if (result.task_title[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Задача: %s", result.task_title);
            } else if (result.command[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Команда: %s", result.command);
            } else if (result.intent[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Интент: %s", result.intent);
            }
            (void)used;
            lv_label_set_text(s_answer, text);
        }

        lv_obj_clear_state(s_mode_button, LV_STATE_DISABLED);
        apply_mode();
        break;
    }
    }
}

void kouki_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_INK), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "K O U K I");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_letter_space(title, 4, 0);
    lv_obj_set_style_text_font(title, &font_ru_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    s_status = lv_label_create(scr);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 300);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &font_ru_16, 0);
    set_status("Подключаюсь…", COLOR_DIM);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 54);

    /* Единственная кнопка действия: начало и конец записи. */
    s_button = lv_btn_create(scr);
    lv_obj_set_size(s_button, 150, 150);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_button, 0, 0);
    lv_obj_align(s_button, LV_ALIGN_CENTER, 0, -24);
    lv_obj_add_event_cb(s_button, on_main_clicked, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_label_set_text(s_button_label, "Записать");
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_font(s_button_label, &font_ru_20, 0);
    lv_obj_center(s_button_label);

    /* Пилюля режима: переключает смысл следующей записи, сама не пишет. */
    s_mode_button = lv_btn_create(scr);
    lv_obj_set_size(s_mode_button, 190, 44);
    lv_obj_set_style_radius(s_mode_button, 22, 0);
    lv_obj_set_style_bg_color(s_mode_button, lv_color_hex(COLOR_PAPER), 0);
    lv_obj_set_style_bg_color(s_mode_button, lv_color_hex(COLOR_DIM), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(s_mode_button, 1, 0);
    lv_obj_set_style_border_color(s_mode_button, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_shadow_width(s_mode_button, 0, 0);
    lv_obj_align(s_mode_button, LV_ALIGN_CENTER, 0, 74);
    lv_obj_add_event_cb(s_mode_button, on_mode_clicked, LV_EVENT_CLICKED, NULL);

    s_mode_label = lv_label_create(s_mode_button);
    lv_label_set_text(s_mode_label, "Режим: задача");
    lv_obj_set_style_text_color(s_mode_label, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_font(s_mode_label, &font_ru_16, 0);
    lv_obj_center(s_mode_label);

    s_answer = lv_label_create(scr);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, 316);
    lv_label_set_text(s_answer, "");
    lv_obj_set_style_text_align(s_answer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_answer, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_font(s_answer, &font_ru_14, 0);
    lv_obj_align(s_answer, LV_ALIGN_BOTTOM_MID, 0, -18);

    apply_mode();
    lv_timer_create(ui_tick, 250, NULL);
}
