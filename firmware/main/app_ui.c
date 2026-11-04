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
#define COLOR_OK       0x38D483
#define COLOR_WARN     0xFFCC33
#define COLOR_DANGER   0xFF4D6D

static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_ask_button;
static lv_obj_t *s_ask_label;
static lv_obj_t *s_status;
static lv_obj_t *s_answer;

static void set_status(const char *text, uint32_t color);

/* Буфер результата живёт статически, чтобы не нагружать стек app_main (+1.7 КБ) */
static kouki_voice_result_t s_tick_result;

/* Срезает недописанный UTF-8 символ в конце строки при обрезке */
static void utf8_trim_tail(char *s)
{
    if (s == NULL) {
        return;
    }
    const size_t len = strlen(s);
    size_t start = len;
    size_t back = 0;
    while (start > 0 && back < 3 && ((unsigned char)s[start - 1] & 0xC0) == 0x80) {
        start--;
        back++;
    }
    if (start == 0) {
        return;
    }
    const unsigned char lead = (unsigned char)s[start - 1];
    size_t need = 0;
    if ((lead & 0x80) == 0) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    if (need > 0 && (len - (start - 1)) < need) {
        s[start - 1] = '\0';
    }
}

static kouki_session_t s_recording_session = KOUKI_SESSION_TASK;
static bool s_is_setup_mode = false;
static char s_setup_text[128] = {0};

void kouki_ui_show_setup(const char *ssid, const char *url)
{
    s_is_setup_mode = true;
    snprintf(s_setup_text, sizeof(s_setup_text), "Wi-Fi: %s\nОткройте %s", ssid, url);
    if (s_status != NULL) {
        set_status("Не настроено", COLOR_WARN);
    }
    if (s_answer != NULL) {
        lv_label_set_text(s_answer, s_setup_text);
    }
    if (s_button != NULL) {
        lv_obj_add_flag(s_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ask_button != NULL) {
        lv_obj_add_flag(s_ask_button, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Начал/отправил: сессия выбирается кнопкой, а не угадывается по словам. */
static void toggle_recording(kouki_session_t session)
{
    if (s_is_setup_mode) {
        return;
    }

    if (kouki_voice_is_recording()) {
        ESP_LOGI(TAG, "останавливаю запись");
        kouki_voice_stop_and_send();
        return;
    }

    if (kouki_voice_set_session(session) != ESP_OK) {
        ESP_LOGW(TAG, "не удалось выбрать сессию — идёт отправка");
        return;
    }

    s_recording_session = session;
    const esp_err_t start_err = kouki_voice_start_recording();
    if (start_err == ESP_ERR_INVALID_STATE) {
        /* Частая причина — станция ещё говорит; экран об этом молчал. */
        ESP_LOGW(TAG, "запись не началась: станция занята (озвучивание или отправка)");
    } else if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "запись не началась: %s", esp_err_to_name(start_err));
    } else {
        ESP_LOGI(TAG, "начинаю запись: %s", session == KOUKI_SESSION_TASK ? "запись" : "вопрос");
    }
}

static void on_task_clicked(lv_event_t *event)
{
    (void)event;
    toggle_recording(KOUKI_SESSION_TASK);
}

static void on_ask_clicked(lv_event_t *event)
{
    (void)event;
    toggle_recording(KOUKI_SESSION_ASK);
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

    if (s_is_setup_mode) {
        set_status("Не настроено", COLOR_WARN);
        if (s_setup_text[0] != '\0') {
            lv_label_set_text(s_answer, s_setup_text);
        }
        return;
    }

    kouki_voice_state_t state = KOUKI_VOICE_IDLE;
    memset(&s_tick_result, 0, sizeof(s_tick_result));
    kouki_voice_get_snapshot(&state, &s_tick_result);

    switch (state) {
    case KOUKI_VOICE_IDLE:
        set_status(kouki_wifi_is_connected() ? "Нажмите и говорите" : "Нет сети", COLOR_DIM);
        lv_label_set_text(s_button_label, "Записать");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_clear_state(s_ask_button, LV_STATE_DISABLED);
        lv_label_set_text(s_ask_label, "Спросить");
        break;

    case KOUKI_VOICE_RECORDING:
        set_status(s_recording_session == KOUKI_SESSION_TASK ? "Слушаю… запишу задачей"
                                                             : "Слушаю… разберу как вопрос",
                   COLOR_WARN);
        lv_label_set_text(s_button_label, "Отправить");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_WARN), 0);
        /* Сессию меняют до записи: во время записи второй кнопки нет. */
        lv_obj_add_state(s_ask_button, LV_STATE_DISABLED);
        break;

    case KOUKI_VOICE_SENDING:
        set_status("Думаю…", COLOR_DIM);
        lv_label_set_text(s_button_label, "…");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_PAPER), 0);
        lv_obj_add_state(s_ask_button, LV_STATE_DISABLED);
        break;

    case KOUKI_VOICE_DONE:
    case KOUKI_VOICE_ERROR: {
        char text[256] = {0};
        size_t used = 0;

        if (s_tick_result.error[0] != '\0') {
            set_status("Не вышло", COLOR_DANGER);
            snprintf(text, sizeof(text), "%s", s_tick_result.error);
            utf8_trim_tail(text);
            lv_label_set_text(s_answer, text);
        } else {
            const char *outcome = s_tick_result.outcome[0] ? s_tick_result.outcome : "executed";
            const bool has_answer = s_tick_result.answer[0] != '\0';
            set_status(has_answer                          ? "Ответ"
                       : strcmp(outcome, "created") == 0   ? "Задача записана"
                       : strcmp(outcome, "executed") == 0  ? "Готово"
                       : "На разбор", COLOR_OK);

            if (s_tick_result.has_transcript && s_tick_result.transcript[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "«%s»\n", s_tick_result.transcript);
            }
            if (has_answer) {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "%s", s_tick_result.answer);
            } else if (s_tick_result.task_title[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Задача: %s", s_tick_result.task_title);
            } else if (s_tick_result.command[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Команда: %s", s_tick_result.command);
            } else if (s_tick_result.intent[0] != '\0') {
                used += (size_t)snprintf(text + used, sizeof(text) - used, "Интент: %s", s_tick_result.intent);
            }
            (void)used;
            utf8_trim_tail(text);
            lv_label_set_text(s_answer, text);
        }

        lv_label_set_text(s_button_label, "Записать");
        lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_clear_state(s_ask_button, LV_STATE_DISABLED);
        lv_label_set_text(s_ask_label, "Спросить");
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

    /* Главная кнопка — сессия «запись». */
    s_button = lv_btn_create(scr);
    lv_obj_set_size(s_button, 150, 150);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_shadow_width(s_button, 0, 0);
    lv_obj_align(s_button, LV_ALIGN_CENTER, 0, -24);
    lv_obj_add_event_cb(s_button, on_task_clicked, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_label_set_text(s_button_label, "Записать");
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_font(s_button_label, &font_ru_20, 0);
    lv_obj_center(s_button_label);

    /* Вторая кнопка — сессия «вопрос»: та же запись, но без подсказки интента. */
    s_ask_button = lv_btn_create(scr);
    lv_obj_set_size(s_ask_button, 190, 44);
    lv_obj_set_style_radius(s_ask_button, 22, 0);
    lv_obj_set_style_bg_color(s_ask_button, lv_color_hex(COLOR_PAPER), 0);
    lv_obj_set_style_bg_color(s_ask_button, lv_color_hex(COLOR_DIM), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(s_ask_button, 1, 0);
    lv_obj_set_style_border_color(s_ask_button, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_shadow_width(s_ask_button, 0, 0);
    lv_obj_align(s_ask_button, LV_ALIGN_CENTER, 0, 74);
    lv_obj_add_event_cb(s_ask_button, on_ask_clicked, LV_EVENT_CLICKED, NULL);

    s_ask_label = lv_label_create(s_ask_button);
    lv_label_set_text(s_ask_label, "Спросить");
    lv_obj_set_style_text_color(s_ask_label, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_font(s_ask_label, &font_ru_16, 0);
    lv_obj_center(s_ask_label);

    s_answer = lv_label_create(scr);
    lv_label_set_long_mode(s_answer, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_answer, 316);
    lv_label_set_text(s_answer, "");
    lv_obj_set_style_text_align(s_answer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_answer, lv_color_hex(COLOR_SAND), 0);
    lv_obj_set_style_text_font(s_answer, &font_ru_14, 0);
    lv_obj_align(s_answer, LV_ALIGN_BOTTOM_MID, 0, -18);

    if (s_is_setup_mode) {
        set_status("Не настроено", COLOR_WARN);
        if (s_setup_text[0] != '\0') {
            lv_label_set_text(s_answer, s_setup_text);
        }
        lv_obj_add_flag(s_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ask_button, LV_OBJ_FLAG_HIDDEN);
    }

    lv_timer_create(ui_tick, 250, NULL);
}
