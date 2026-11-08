#pragma once

#include "app_voice.h"

namespace kouki::voice {

class VoiceResponseParser {
public:
    // Разбирает JSON-ответ от сервера POST /api/voice в структуру kouki_voice_result_t.
    // Возвращает true, если JSON успешно распарсен (даже если внутри сервер сообщил об ошибке).
    static bool parse(const char *json_str, kouki_voice_result_t *out) noexcept;
};

} // namespace kouki::voice
