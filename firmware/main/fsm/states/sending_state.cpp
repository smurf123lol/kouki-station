#include "sending_state.hpp"
#include "fsm/voice_response_parser.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifdef ESP_PLATFORM
#include "app_tone.h"
#include "app_config.h"
#include "app_http.h"
#include "app_wifi.h"
#include "app_ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
static const char *TAG = "kouki.fsm.send";
#endif

namespace kouki::voice {

SendingState::SendingState() {
#ifdef ESP_PLATFORM
    m_client_lock = xSemaphoreCreateMutex();
#endif
}

SendingState::~SendingState() {
#ifdef ESP_PLATFORM
    if (m_client_lock) {
        vSemaphoreDelete(m_client_lock);
        m_client_lock = nullptr;
    }
#endif
}

#ifdef ESP_PLATFORM

struct SendWorkerCtx {
    SendingState *state;
    VoiceContext *ctx;
};

void SendingState::send_worker_task(void *arg) {
    auto *worker_ctx = static_cast<SendWorkerCtx *>(arg);
    SendingState *self = worker_ctx->state;
    VoiceContext *ctx = worker_ctx->ctx;
    delete worker_ctx;

    if (!self || !ctx) {
        vTaskDelete(nullptr);
        return;
    }

    if (self->m_cancelled.load()) {
        self->notify_complete(StateId::Idle);
        vTaskDelete(nullptr);
        return;
    }

    kouki_tone_play(KOUKI_TONE_STOP_SENDING);
    if (self->m_cancelled.load()) {
        self->notify_complete(StateId::Idle);
        vTaskDelete(nullptr);
        return;
    }

    if (!kouki_wifi_is_connected()) {
        std::strncpy(ctx->result.error, "Нет сети", sizeof(ctx->result.error) - 1);
        ctx->result.error[sizeof(ctx->result.error) - 1] = '\0';
        self->notify_complete(StateId::Error);
        vTaskDelete(nullptr);
        return;
    }

    const char *srv_url = kouki_config_get_srv_url();
    const char *dev_source = kouki_config_get_dev_source();
    const char *srv_token = kouki_config_get_srv_token();

    char url[300];
    if (ctx->dialogue_continue) {
        std::snprintf(url, sizeof(url), "%s/api/voice?source=%s&dialogue=continue", srv_url, dev_source);
        ctx->dialogue_continue = false;
    } else {
        std::snprintf(url, sizeof(url), "%s/api/voice?source=%s", srv_url, dev_source);
    }

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 4096;

    esp_http_client_handle_t client = nullptr;
    esp_err_t err = kouki_http_new_client(&cfg, &client);
    if (err != ESP_OK || !client) {
        std::strncpy(ctx->result.error, "Ошибка создания HTTP клиента", sizeof(ctx->result.error) - 1);
        ctx->result.error[sizeof(ctx->result.error) - 1] = '\0';
        self->notify_complete(StateId::Error);
        vTaskDelete(nullptr);
        return;
    }

    if (self->m_client_lock) xSemaphoreTake(self->m_client_lock, portMAX_DELAY);
    self->m_http_client = client;
    if (self->m_client_lock) xSemaphoreGive(self->m_client_lock);

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    if (srv_token && std::strlen(srv_token) > 0) {
        char auth[300];
        std::snprintf(auth, sizeof(auth), "Bearer %s", srv_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    size_t post_len = WAV_HEADER_SIZE + ctx->sample_bytes;
    esp_http_client_set_post_field(client, reinterpret_cast<const char *>(ctx->buffer), static_cast<int>(post_len));

    constexpr size_t RESP_MAX_LEN = 16384;
    char *response = static_cast<char *>(heap_caps_malloc(RESP_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!response) {
        response = static_cast<char *>(std::malloc(RESP_MAX_LEN));
    }
    if (response) {
        response[0] = '\0';
    }

    err = esp_http_client_perform(client);

    if (self->m_client_lock) xSemaphoreTake(self->m_client_lock, portMAX_DELAY);
    self->m_http_client = nullptr;
    if (self->m_client_lock) xSemaphoreGive(self->m_client_lock);

    if (self->m_cancelled.load()) {
        esp_http_client_cleanup(client);
        if (response) std::free(response);
        self->notify_complete(StateId::Idle);
        vTaskDelete(nullptr);
        return;
    }

    int status = esp_http_client_get_status_code(client);
    ctx->result.http_status = status;

    if (err == ESP_OK && response) {
        int read_len = esp_http_client_read_response(client, response, RESP_MAX_LEN - 1);
        if (read_len >= 0) {
            response[read_len] = '\0';
        }
    }

    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300) {
        if (status > 0) {
            std::snprintf(ctx->result.error, sizeof(ctx->result.error), "Сервер ответил HTTP %d", status);
        } else {
            std::strncpy(ctx->result.error, "Нет связи с сервером.", sizeof(ctx->result.error) - 1);
            ctx->result.error[sizeof(ctx->result.error) - 1] = '\0';
        }
        ESP_LOGW(TAG, "Ошибка HTTP (%d, err=%s): %s", status, esp_err_to_name(err), response ? response : "");
        self->notify_complete(StateId::Error);
    } else {
        // T111: Подтверждение валидности образа OTA при успешном ответе сервера
        kouki_ota_mark_server_communicated();

        // Парсинг через вынесенный объект VoiceResponseParser
        if (response) {
            VoiceResponseParser::parse(response, &ctx->result);
        }
        if (ctx->result.error[0] != '\0') {
            self->notify_complete(StateId::Error);
        } else {
            self->notify_complete(StateId::Done);
        }
    }

    if (response) std::free(response);
    vTaskDelete(nullptr);
}
#endif

void SendingState::on_enter(VoiceContext& ctx, const char* /*reason*/) {
    m_in_progress = true;
    m_finished = false;
    m_cancelled = false;
    m_next_state = StateId::Stay;

#ifdef ESP_PLATFORM
    if (!ctx.is_loud_enough(CONFIG_KOUKI_MIN_AMPLITUDE)) {
        std::strncpy(ctx.result.error, "Тишина: слишком тихо, повторите ближе к микрофону",
                     sizeof(ctx.result.error) - 1);
        ctx.result.error[sizeof(ctx.result.error) - 1] = '\0';
        notify_complete(StateId::Error);
        return;
    }

    auto *worker_ctx = new SendWorkerCtx{this, &ctx};
    if (xTaskCreate(send_worker_task, "fsm.send", 8192, worker_ctx, 5, &m_task_handle) != pdPASS) {
        delete worker_ctx;
        std::strncpy(ctx.result.error, "Не удалось запустить отправку", sizeof(ctx.result.error) - 1);
        ctx.result.error[sizeof(ctx.result.error) - 1] = '\0';
        notify_complete(StateId::Error);
    }
#else
    if (!ctx.is_loud_enough(120)) {
        notify_complete(StateId::Error);
    }
    // Stay in Sending on host until notify_complete() or cancel
#endif
}

StateId SendingState::on_step(VoiceContext& /*ctx*/, const int16_t* /*pcm*/, size_t /*samples*/) {
    if (m_finished.load()) {
        return m_next_state.load();
    }
    return StateId::Stay;
}

void SendingState::on_interrupt(VoiceContext& /*ctx*/, const char* reason) {
    if (!reason || std::strcmp(reason, "cancel") != 0) {
        return;
    }
    m_cancelled = true;
#ifdef ESP_PLATFORM
    if (m_client_lock && xSemaphoreTake(m_client_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (m_http_client) {
            esp_http_client_close(m_http_client);
        }
        xSemaphoreGive(m_client_lock);
    }
#else
    notify_complete(StateId::Idle);
#endif
}

void SendingState::on_exit(VoiceContext& /*ctx*/) {
    m_in_progress = false;
}

void SendingState::notify_complete(StateId result_state) noexcept {
    m_next_state = result_state;
    m_finished = true;
}

} // namespace kouki::voice
