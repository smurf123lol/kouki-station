#include "sending_state.hpp"
#include "fsm/voice_response_parser.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <new>

#ifdef ESP_PLATFORM
#include "app_tone.h"
#include "app_config.h"
#include "app_http.h"
#include "app_wifi.h"
#include "app_ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_client.h"
static const char *TAG = "kouki.fsm.send";
#endif

namespace kouki::voice {

#ifdef ESP_PLATFORM

struct SendWorkerCtx {
    SendingState *state;
    uint8_t *wav_buffer;
    size_t post_len;
    uint32_t gen;
    uint32_t duration_ms;
    bool dialogue_continue;
};

void SendingState::send_worker_task(void *arg) {
    auto *worker_ctx = static_cast<SendWorkerCtx *>(arg);
    SendingState *self = worker_ctx ? worker_ctx->state : nullptr;
    uint8_t *wav_buf = worker_ctx ? worker_ctx->wav_buffer : nullptr;
    const size_t post_len = worker_ctx ? worker_ctx->post_len : 0;
    const uint32_t gen = worker_ctx ? worker_ctx->gen : 0;
    const uint32_t duration_ms = worker_ctx ? worker_ctx->duration_ms : 0;
    const bool dialogue_continue = worker_ctx ? worker_ctx->dialogue_continue : false;
    delete worker_ctx;

    if (!self || self->m_gen.load(std::memory_order_acquire) != gen || self->m_cancelled.load(std::memory_order_acquire)) {
        if (self && self->m_gen.load(std::memory_order_acquire) == gen) {
            self->notify_complete(StateId::Idle);
        }
        vTaskDelete(nullptr);
        return;
    }

    kouki_tone_play(KOUKI_TONE_STOP_SENDING);

    if (self->m_gen.load(std::memory_order_acquire) != gen || self->m_cancelled.load(std::memory_order_acquire)) {
        if (self->m_gen.load(std::memory_order_acquire) == gen) {
            self->notify_complete(StateId::Idle);
        }
        vTaskDelete(nullptr);
        return;
    }

    kouki_voice_result_t local_res{};
    local_res.duration_ms = duration_ms;

    if (!kouki_wifi_is_connected()) {
        std::strncpy(local_res.error, "Нет сети", sizeof(local_res.error) - 1);
        local_res.error[sizeof(local_res.error) - 1] = '\0';
        if (self->m_gen.load(std::memory_order_acquire) == gen && !self->m_cancelled.load(std::memory_order_acquire)) {
            self->m_worker_result = local_res;
            self->m_has_worker_result.store(true, std::memory_order_release);
            self->notify_complete(StateId::Error);
        }
        vTaskDelete(nullptr);
        return;
    }

    const char *srv_url = kouki_config_get_srv_url();
    const char *dev_source = kouki_config_get_dev_source();
    const char *srv_token = kouki_config_get_srv_token();

    char url[300];
    if (dialogue_continue) {
        std::snprintf(url, sizeof(url), "%s/api/voice?source=%s&dialogue=continue", srv_url, dev_source);
    } else {
        std::snprintf(url, sizeof(url), "%s/api/voice?source=%s", srv_url, dev_source);
    }

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;
    cfg.buffer_size = 4096;

    esp_http_client_handle_t client = nullptr;
    esp_err_t err = kouki_http_new_client(&cfg, &client);
    if (err != ESP_OK || !client) {
        std::strncpy(local_res.error, "Ошибка создания HTTP клиента", sizeof(local_res.error) - 1);
        local_res.error[sizeof(local_res.error) - 1] = '\0';
        if (self->m_gen.load(std::memory_order_acquire) == gen && !self->m_cancelled.load(std::memory_order_acquire)) {
            self->m_worker_result = local_res;
            self->m_has_worker_result.store(true, std::memory_order_release);
            self->notify_complete(StateId::Error);
        }
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    if (srv_token && std::strlen(srv_token) > 0) {
        char auth[300];
        std::snprintf(auth, sizeof(auth), "Bearer %s", srv_token);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    err = esp_http_client_open(client, static_cast<int>(post_len));
    if (err != ESP_OK) {
        kouki_http_diagnose_error(client, url, err, local_res.error, sizeof(local_res.error));
        esp_http_client_cleanup(client);
        if (self->m_gen.load(std::memory_order_acquire) == gen && !self->m_cancelled.load(std::memory_order_acquire)) {
            self->m_worker_result = local_res;
            self->m_has_worker_result.store(true, std::memory_order_release);
            self->notify_complete(StateId::Error);
        }
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_set_timeout_ms(client, 2000);
    size_t sent = 0;
    bool write_failed = false;
    while (sent < post_len) {
        if (self->m_gen.load(std::memory_order_acquire) != gen || self->m_cancelled.load(std::memory_order_acquire)) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (self->m_gen.load(std::memory_order_acquire) == gen) {
                self->notify_complete(StateId::Idle);
            }
            vTaskDelete(nullptr);
            return;
        }
        const size_t left = post_len - sent;
        const size_t chunk = (left < 4096u) ? left : 4096u;
        const int w = esp_http_client_write(client, reinterpret_cast<const char *>(wav_buf + sent), static_cast<int>(chunk));
        if (w <= 0) {
            write_failed = true;
            break;
        }
        sent += static_cast<size_t>(w);
    }

    if (write_failed) {
        kouki_http_diagnose_error(client, url, ESP_FAIL, local_res.error, sizeof(local_res.error));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (self->m_gen.load(std::memory_order_acquire) == gen && !self->m_cancelled.load(std::memory_order_acquire)) {
            self->m_worker_result = local_res;
            self->m_has_worker_result.store(true, std::memory_order_release);
            self->notify_complete(StateId::Error);
        }
        vTaskDelete(nullptr);
        return;
    }

    esp_http_client_set_timeout_ms(client, 1000);
    int64_t hdr_len = -1;
    int hdr_wait_ms = 0;
    while (true) {
        if (self->m_gen.load(std::memory_order_acquire) != gen || self->m_cancelled.load(std::memory_order_acquire)) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (self->m_gen.load(std::memory_order_acquire) == gen) {
                self->notify_complete(StateId::Idle);
            }
            vTaskDelete(nullptr);
            return;
        }
        hdr_len = esp_http_client_fetch_headers(client);
        if (hdr_len == -ESP_ERR_HTTP_EAGAIN) {
            hdr_wait_ms += 1000;
            if (hdr_wait_ms >= 30000) {
                break;
            }
            continue;
        }
        break;
    }

    const int status = esp_http_client_get_status_code(client);
    local_res.http_status = status;

    constexpr size_t RESP_MAX_LEN = 16384;
    char *response = nullptr;
    if (status >= 200 && status < 300) {
        response = static_cast<char *>(heap_caps_malloc(RESP_MAX_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!response) {
            response = static_cast<char *>(std::malloc(RESP_MAX_LEN));
        }
        if (response) {
            response[0] = '\0';
        }
    }

    esp_http_client_set_timeout_ms(client, 250);
    size_t total_read = 0;
    int read_wait_ms = 0;
    bool read_err = false;
    char devnull[64];

    while (true) {
        if (self->m_gen.load(std::memory_order_acquire) != gen || self->m_cancelled.load(std::memory_order_acquire)) {
            if (response) std::free(response);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            if (self->m_gen.load(std::memory_order_acquire) == gen) {
                self->notify_complete(StateId::Idle);
            }
            vTaskDelete(nullptr);
            return;
        }

        char *dst = response ? (response + total_read) : devnull;
        const int avail = response ? static_cast<int>(RESP_MAX_LEN - 1 - total_read) : static_cast<int>(sizeof(devnull));
        if (avail <= 0) {
            break;
        }

        const int r = esp_http_client_read(client, dst, avail);
        if (r > 0) {
            if (response) total_read += static_cast<size_t>(r);
            read_wait_ms = 0;
        } else if (r == 0) {
            break;
        } else if (r == -ESP_ERR_HTTP_EAGAIN) {
            read_wait_ms += 250;
            if (read_wait_ms >= 15000) {
                read_err = true;
                break;
            }
        } else {
            read_err = true;
            break;
        }
    }

    if (response) {
        response[total_read] = '\0';
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (self->m_gen.load(std::memory_order_acquire) != gen || self->m_cancelled.load(std::memory_order_acquire)) {
        if (response) std::free(response);
        if (self->m_gen.load(std::memory_order_acquire) == gen) {
            self->notify_complete(StateId::Idle);
        }
        vTaskDelete(nullptr);
        return;
    }

    if (hdr_len < 0 || read_err || status < 200 || status >= 300) {
        if (status > 0) {
            std::snprintf(local_res.error, sizeof(local_res.error), "Сервер ответил HTTP %d", status);
        } else {
            std::strncpy(local_res.error, "Нет связи с сервером.", sizeof(local_res.error) - 1);
            local_res.error[sizeof(local_res.error) - 1] = '\0';
        }
        ESP_LOGW(TAG, "Ошибка HTTP (%d, read_err=%d): %s", status, read_err, response ? response : "");
        self->m_worker_result = local_res;
        self->m_has_worker_result.store(true, std::memory_order_release);
        self->notify_complete(StateId::Error);
    } else {
        kouki_ota_mark_server_communicated();

        if (response && response[0] != '\0') {
            VoiceResponseParser::parse(response, &local_res);
        } else {
            std::strncpy(local_res.error, "Пустой ответ сервера", sizeof(local_res.error) - 1);
            local_res.error[sizeof(local_res.error) - 1] = '\0';
        }

        self->m_worker_result = local_res;
        self->m_has_worker_result.store(true, std::memory_order_release);
        if (local_res.error[0] != '\0') {
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
    m_has_worker_result = false;
    m_next_state = StateId::Stay;
    m_worker_result = {};
    m_worker_result.duration_ms = ctx.result.duration_ms;

    const uint32_t gen = m_gen.fetch_add(1, std::memory_order_acq_rel) + 1;

#ifdef ESP_PLATFORM
    if (!ctx.is_loud_enough(CONFIG_KOUKI_MIN_AMPLITUDE)) {
        std::strncpy(ctx.result.error, "Тишина: слишком тихо, повторите ближе к микрофону",
                     sizeof(ctx.result.error) - 1);
        ctx.result.error[sizeof(ctx.result.error) - 1] = '\0';
        notify_complete(StateId::Error);
        return;
    }

    const size_t post_len = WAV_HEADER_SIZE + ctx.sample_bytes;
    const bool dialogue_continue = ctx.dialogue_continue;
    ctx.dialogue_continue = false;

    auto *worker_ctx = new (std::nothrow) SendWorkerCtx{
        this,
        ctx.buffer,
        post_len,
        gen,
        ctx.result.duration_ms,
        dialogue_continue
    };
    if (!worker_ctx || xTaskCreate(send_worker_task, "fsm.send", 8192, worker_ctx, 5, &m_task_handle) != pdPASS) {
        delete worker_ctx;
        std::strncpy(ctx.result.error, "Не удалось запустить отправку", sizeof(ctx.result.error) - 1);
        ctx.result.error[sizeof(ctx.result.error) - 1] = '\0';
        notify_complete(StateId::Error);
    }
#else
    (void)gen;
    if (!ctx.is_loud_enough(120)) {
        notify_complete(StateId::Error);
    }
#endif
}

StateId SendingState::on_step(VoiceContext& ctx, const int16_t* /*pcm*/, size_t /*samples*/) {
    if (m_finished.load(std::memory_order_acquire)) {
        if (m_cancelled.load(std::memory_order_acquire)) {
            return StateId::Idle;
        }
        if (m_has_worker_result.load(std::memory_order_acquire)) {
            ctx.result = m_worker_result;
        }
        return m_next_state.load(std::memory_order_acquire);
    }
    return StateId::Stay;
}

void SendingState::on_interrupt(VoiceContext& /*ctx*/, const char* reason) {
    if (!reason || std::strcmp(reason, "cancel") != 0) {
        return;
    }
    m_cancelled = true;
#ifndef ESP_PLATFORM
    notify_complete(StateId::Idle);
#endif
}

void SendingState::on_exit(VoiceContext& /*ctx*/) {
    m_in_progress = false;
}

void SendingState::notify_complete(StateId result_state) noexcept {
    m_next_state.store(result_state, std::memory_order_release);
    m_finished.store(true, std::memory_order_release);
}

} // namespace kouki::voice
