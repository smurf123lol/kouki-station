#include "voice_response_parser.hpp"
#include <cstring>

#ifdef ESP_PLATFORM
#include "cJSON.h"
#else
// Для host-тестов, если cJSON подключен через extern или мини-парсер
#if __has_include("cJSON.h")
#include "cJSON.h"
#endif
#endif

namespace kouki::voice {

static void trim_utf8_tail(char *s) noexcept {
    if (!s || s[0] == '\0') return;
    const size_t len = std::strlen(s);
    const auto *u = reinterpret_cast<const unsigned char *>(s);
    size_t i = len;
    while (i > 0 && (u[i - 1] & 0xC0u) == 0x80u) {
        i--;
    }
    if (i == len) {
        if ((u[len - 1] & 0x80u) != 0u) {
            s[len - 1] = '\0';
        }
        return;
    }
    if (i == 0) {
        s[0] = '\0';
        return;
    }
    const unsigned char lead = u[i - 1];
    size_t expected = 0;
    if ((lead & 0xE0u) == 0xC0u) {
        expected = 2;
    } else if ((lead & 0xF0u) == 0xE0u) {
        expected = 3;
    } else if ((lead & 0xF8u) == 0xF0u) {
        expected = 4;
    } else {
        s[lead < 0x80u ? i : (i - 1)] = '\0';
        return;
    }
    if ((len - (i - 1)) < expected) {
        s[i - 1] = '\0';
    }
}

static void copy_field(char *dst, size_t dst_len, const char *src) noexcept {
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
    trim_utf8_tail(dst);
}

#if defined(ESP_PLATFORM) || __has_include("cJSON.h")
static const char* get_str(const cJSON *obj, const char *key) noexcept {
    if (!obj || !key) return nullptr;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : nullptr;
}
#endif

bool VoiceResponseParser::parse(const char *json_str, kouki_voice_result_t *out) noexcept {
    if (!out) return false;
    if (!json_str || json_str[0] == '\0') {
        copy_field(out->error, sizeof(out->error), "Пустой ответ сервера");
        return false;
    }

#if defined(ESP_PLATFORM) || __has_include("cJSON.h")
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        copy_field(out->error, sizeof(out->error), "Сервер ответил не-JSON");
        return false;
    }

    const cJSON *intake = cJSON_GetObjectItemCaseSensitive(root, "intake");
    const char *err = get_str(root, "error");

    copy_field(out->transcript, sizeof(out->transcript), get_str(root, "transcript"));
    out->has_transcript = (out->transcript[0] != '\0');

    if (err) {
        copy_field(out->error, sizeof(out->error), err);
    }

    if (cJSON_IsObject(intake)) {
        const cJSON *intent = cJSON_GetObjectItemCaseSensitive(intake, "intent");
        if (cJSON_IsObject(intent)) {
            copy_field(out->intent, sizeof(out->intent), get_str(intent, "intent"));
        }
        copy_field(out->outcome, sizeof(out->outcome), get_str(intake, "outcome"));
        copy_field(out->command, sizeof(out->command), get_str(intake, "command"));

        const cJSON *task = cJSON_GetObjectItemCaseSensitive(intake, "task");
        if (cJSON_IsObject(task)) {
            copy_field(out->task_title, sizeof(out->task_title), get_str(task, "title"));
            copy_field(out->task_status, sizeof(out->task_status), get_str(task, "status"));
        }

        const cJSON *res = cJSON_GetObjectItemCaseSensitive(intake, "result");
        if (cJSON_IsObject(res)) {
            copy_field(out->answer, sizeof(out->answer), get_str(res, "answer"));
            if (out->task_title[0] == '\0') {
                copy_field(out->task_title, sizeof(out->task_title), get_str(res, "question"));
            }
            const cJSON *req_conf = cJSON_GetObjectItemCaseSensitive(res, "requiresConfirmation");
            if (cJSON_IsTrue(req_conf)) {
                out->requires_confirmation = true;
            }
        }

        const char *intake_err = get_str(intake, "error");
        if (intake_err && out->error[0] == '\0') {
            copy_field(out->error, sizeof(out->error), intake_err);
        }
    }

    cJSON_Delete(root);
    return true;
#else
    // Заглушка для сред без cJSON
    copy_field(out->transcript, sizeof(out->transcript), "host test transcript");
    out->has_transcript = true;
    return true;
#endif
}

} // namespace kouki::voice
