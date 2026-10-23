#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "app_wifi.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "kouki.ui";

/* Палитра та же, что у дашборда: бумага, чернила, вермильон. */
#define COLOR_INK      0x0B0A08
#define COLOR_PAPER    0x16130F
#define COLOR_SAND     0xE2D2B2
#define COLOR_DIM      0x8B7D66
#define COLOR_ACCENT   0xD8552F
#define COLOR_OK       0x38D483
#define COLOR_WARN     0xFFCC33
#define COLOR_DANGER   0xFF4D6D

static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_status;
static lv_obj_t *s_answer;

static void on_button_clicked(lv_event_t *event)
{
    (void)event;
    if (kouki_voice_is_recording()) {
        ESP_LOGI(TAG, "останавливаю запись");
        kouki_voice_stop_and_send();
    } else {
        ESP_LOGI(TAG, "начинаю запись");
        kouki_voice_start_recording();
    }
}

static void set_status(const char *text, uint32_t color)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
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
        lv_label_set_text(s_button_label, "Спросить");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
        break;

    case KOUKI_VOICE_RECORDING:
        set_status("Слушаю… нажмите, чтобы отправить", COLOR_WARN);
        lv_label_set_text(s_button_label, "Отправить");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_WARN), 0);
        break;

    case KOUKI_VOICE_SENDING:
        set_status("Думаю…", COLOR_DIM);
        lv_label_set_text(s_button_label, "…");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_PAPER), 0);
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
            set_status(strcmp(outcome, "created") == 0   ? "Задача записана"
                       : strcmp(outcome, "executed") == 0 ? "Готово"
                       : "На разбор", COLOR_OK);

            if (result.has_transcript && result.transcript[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "«%s»\n", result.transcript);
            }
            if (result.task_title[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Задача: %s", result.task_title);
            } else if (result.command[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Команда: %s", result.command);
            } else if (result.intent[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Интент: %s", result.intent);
            }
            lv_label_set_text(s_answer, text);
        }

        lv_label_set_text(s_button_label, "Спросить");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
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
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    s_status = lv_label_create(scr);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 300);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, 0);
    set_status("Подключаюсь…", COLOR_DIM);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 62);

    s_button = lv_btn_create(scr);
    lv_obj_set_size(s_button, 168, 168);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_button, 0, 0);
    lv_obj_align(s_button, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_event_cb(s_button, on_button_clicked, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_label_set_text(s_button_label, "Спросить");
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_font(s_button_label, &lv_font_montserrat_20, 0);
    lv_obj_center(s_button_label);

    s_answer = lv_label_create(scr);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, 300);
    lv_label_set_text(s_answer, "");
    lv_obj_set_style_text_align(s_answer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_answer, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_font(s_answer, &lv_font_montserrat_14, 0);
    lv_obj_align(s_answer, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_timer_create(ui_tick, 250, NULL);
}
