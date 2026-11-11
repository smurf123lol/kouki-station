#include "app_wake_verify.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app_config.hpp"
#include "app_http.hpp"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace kouki {
namespace {

const char *const TAG = "kouki.wake.verify";

/** Таймаут ASR-проверки: значение из Kconfig, стенд подставляет своё. */
#ifndef CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS
#define CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS 4000
#endif

/** Предел тела ответа /api/transcribe, которое мы разбираем. */
constexpr size_t kMaxBody = 512;

/** Размер WAV-заголовка PCM 16 бит/моно: RIFF+fmt+data. */
constexpr size_t kWavHeaderBytes = 44;

/** Размер буфера-чанка при отправке WAV. */
constexpr size_t kWriteChunk = 4096;

/** Копирует 16-битное значение little-endian. */
void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/** Копирует 32-битное значение little-endian. */
void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/** Пишет минимальный WAV-заголовок: RIFF/WAVE, fmt 16, PCM, моно, 16 кГц, 16 бит. */
void write_wav_header(uint8_t *buf, size_t pcm_bytes)
{
    memcpy(buf + 0, "RIFF", 4);
    put_le32(buf + 4, (uint32_t)(36 + pcm_bytes));
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    put_le32(buf + 16, 16);
    put_le16(buf + 20, 1);      /* PCM */
    put_le16(buf + 22, 1);      /* моно */
    put_le32(buf + 24, 16000);  /* частота */
    put_le32(buf + 28, 16000 * 2);
    put_le16(buf + 32, 2);      /* выравнивание */
    put_le16(buf + 34, 16);     /* бит на сэмпл */
    memcpy(buf + 36, "data", 4);
    put_le32(buf + 40, (uint32_t)pcm_bytes);
}

/** true, если подстрока встречается в строке (обе непустые). */
bool contains(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

/** Переводит ASCII и кириллицу UTF-8 в нижний регистр (правка на месте).
    Whisper пишет обращения с заглавной («Эй, Динь»), а needles у нас строчные. */
void lower_utf8(char *s)
{
    for (size_t i = 0; s[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') {
            s[i] = (char)(c - 'A' + 'a');
            continue;
        }
        if ((c == 0xD0 || c == 0xD1) && s[i + 1] != '\0') {
            const unsigned char n = (unsigned char)s[i + 1];
            if (c == 0xD0 && n == 0x81) {           /* Ё -> ё */
                s[i] = (char)0xD1;
                s[i + 1] = (char)0x91;
                ++i;
                continue;
            }
            if (c == 0xD0 && n >= 0x90 && n <= 0xAF) { /* А-Я -> а-я */
                const unsigned cp = (((unsigned)(c & 0x1F)) << 6) | (unsigned)(n & 0x3F);
                const unsigned low = cp + 0x20;         /* 0x0410..0x042F -> 0x0430..0x044F */
                s[i] = (char)(0xC0 | (low >> 6));
                s[i + 1] = (char)(0x80 | (low & 0x3F));
                ++i;
                continue;
            }
            ++i;                                     /* двухбайтовый символ без изменений */
        }
    }
}

/* Достаёт значение поля "transcript" из JSON без полноценного парсера:
   для двух полей этого достаточно, а зависимость на cJSON в этой задаче не нужна. */
bool extract_transcript(const char *json, char *out, size_t out_len)
{
    if (!json || !out || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *key = strstr(json, "\"transcript\"");
    if (!key) {
        return false;
    }
    const char *colon = strchr(key, ':');
    if (!colon) {
        return false;
    }
    const char *q = strchr(colon, '"');
    if (!q) {
        return false;
    }
    ++q;

    size_t written = 0;
    while (*q && *q != '"' && written + 1 < out_len) {
        if (*q == '\\' && (q[1] == '"' || q[1] == '\\')) {
            out[written++] = q[1];
            q += 2;
            continue;
        }
        /* \uXXXX и прочие escape-последовательности не разворачиваем:
           для сверки хватит и «сырых» байтов. */
        out[written++] = *q++;
    }
    out[written] = '\0';
    return true;
}

}  // namespace

WakeVerify::WakeVerify(Config &config, Http &http) : m_config(config), m_http(http) {}

bool WakeVerify::matches(const char *transcript)
{
    if (!transcript || transcript[0] == '\0') {
        return false;
    }

    /* Сверка без учёта регистра: «Эй, Динь» должен пройти так же, как «эй гин». */
    char lowered[128];
    strncpy(lowered, transcript, sizeof(lowered) - 1);
    lowered[sizeof(lowered) - 1] = '\0';
    lower_utf8(lowered);
    transcript = lowered;

    /* Whisper на живой записи почти никогда не пишет «эй гин» дословно:
       в замерах встречались «Эй, Дин!», «Эдит!», «Пейдем!», «Рейдин, покой…».
       Поэтому принимаем, если в тексте есть характерное слово семейства
       «гин/дин/жин», либо текст начинается с короткого обращения. */
    const bool has_word = contains(transcript, "гин") || contains(transcript, "гын") ||
                          contains(transcript, "гим") || contains(transcript, "жин") ||
                          contains(transcript, "джин") || contains(transcript, "дин") ||
                          contains(transcript, "дим") || contains(transcript, "ден") ||
                          contains(transcript, "дем") || contains(transcript, "дан");
    if (has_word) {
        return true;
    }

    const size_t len = strlen(transcript);
    if (len > 16) {
        return false;
    }

    const bool has_prefix = contains(transcript, "эй") || contains(transcript, "ей") ||
                            contains(transcript, "ай") || contains(transcript, "шей") ||
                            contains(transcript, "рей") || contains(transcript, "бей") ||
                            contains(transcript, "гей") || contains(transcript, "пей") ||
                            contains(transcript, "дей") || contains(transcript, "эд") ||
                            contains(transcript, "ед");
    return has_prefix;
}

void WakeVerify::task_entry(void *arg)
{
    static_cast<WakeVerify *>(arg)->run();
    vTaskDelete(NULL);
}

void WakeVerify::run()
{
    Job *job = m_job;
    int result = KOUKI_WAKE_VERIFY_UNAVAILABLE;
    m_transcript[0] = '\0';

    if (job && job->wav && job->wav_len > kWavHeaderBytes) {
        const char *srv_url = m_config.srv_url();
        const char *srv_token = m_config.srv_token();

        char url[300];
        snprintf(url, sizeof(url), "%s/api/transcribe", srv_url ? srv_url : "");

        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = HTTP_METHOD_POST;
        cfg.timeout_ms = CONFIG_KOUKI_WAKE_VERIFY_TIMEOUT_MS;
        cfg.buffer_size = 4096;

        esp_http_client_handle_t client = NULL;
        esp_err_t err = m_http.new_client(&cfg, &client);
        if (err == ESP_OK && client) {
            esp_http_client_set_header(client, "Content-Type", "audio/wav");
            if (srv_token && srv_token[0] != '\0') {
                char auth[300];
                snprintf(auth, sizeof(auth), "Bearer %s", srv_token);
                esp_http_client_set_header(client, "Authorization", auth);
            }

            err = esp_http_client_open(client, (int)job->wav_len);
            if (err == ESP_OK) {
                size_t sent = 0;
                while (sent < job->wav_len) {
                    size_t left = job->wav_len - sent;
                    int chunk = (left < kWriteChunk) ? (int)left : (int)kWriteChunk;
                    int w = esp_http_client_write(client, (const char *)(job->wav + sent), chunk);
                    if (w <= 0) {
                        err = ESP_FAIL;
                        break;
                    }
                    sent += (size_t)w;
                }
            }

            if (err == ESP_OK) {
                int64_t hdr = esp_http_client_fetch_headers(client);
                const int status = esp_http_client_get_status_code(client);
                char body[kMaxBody];
                size_t total = 0;
                body[0] = '\0';

                if (hdr >= 0) {
                    while (total + 1 < sizeof(body)) {
                        int r = esp_http_client_read(client, body + total, (int)(sizeof(body) - 1 - total));
                        if (r <= 0) {
                            break;
                        }
                        total += (size_t)r;
                    }
                    body[total] = '\0';
                }

                if (status >= 200 && status < 300) {
                    if (extract_transcript(body, m_transcript, sizeof(m_transcript))) {
                        result = matches(m_transcript)
                                     ? KOUKI_WAKE_VERIFY_ACCEPTED
                                     : KOUKI_WAKE_VERIFY_REJECTED;
                        ESP_LOGI(TAG, "ASR-подтверждение: «%s» -> %s", m_transcript,
                                 result == KOUKI_WAKE_VERIFY_ACCEPTED ? "принято" : "отклонено");
                    } else {
                        ESP_LOGW(TAG, "в ответе нет transcript: %s", body);
                    }
                } else if (status == 422) {
                    result = KOUKI_WAKE_VERIFY_REJECTED;
                    ESP_LOGI(TAG, "ASR не распознал речь — хит отклонён");
                } else {
                    ESP_LOGW(TAG, "ASR-проверка: HTTP %d", status);
                }
            } else {
                char diag[96] = {0};
                m_http.diagnose_error(client, url, err, diag, sizeof(diag));
                ESP_LOGW(TAG, "ASR-проверка недоступна: %s", diag);
            }

            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        } else {
            ESP_LOGW(TAG, "не удалось создать HTTP-клиент для ASR-проверки");
            if (client) {
                esp_http_client_cleanup(client);
            }
        }
    }

    if (job) {
        free(job->wav);
        free(job);
        m_job = nullptr;
    }

    m_state.store(result, std::memory_order_release);
    m_task = nullptr;
}

esp_err_t WakeVerify::start(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m_task != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t pcm_bytes = (size_t)samples * 2u;
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t *wav = (uint8_t *)heap_caps_malloc(kWavHeaderBytes + pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) {
        wav = (uint8_t *)malloc(kWavHeaderBytes + pcm_bytes);
    }
    if (!wav) {
        free(job);
        return ESP_ERR_NO_MEM;
    }

    write_wav_header(wav, pcm_bytes);
    memcpy(wav + kWavHeaderBytes, pcm, pcm_bytes);

    job->wav = wav;
    job->wav_len = kWavHeaderBytes + pcm_bytes;
    m_job = job;

    m_state.store(KOUKI_WAKE_VERIFY_PENDING, std::memory_order_release);
    m_transcript[0] = '\0';

    if (xTaskCreate(task_entry, "wake_verify", 8192, this, tskIDLE_PRIORITY + 1, &m_task) != pdPASS) {
        m_task = nullptr;
        m_state.store(KOUKI_WAKE_VERIFY_UNAVAILABLE, std::memory_order_release);
        m_job = nullptr;
        free(wav);
        free(job);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

kouki_wake_verify_state_t WakeVerify::state() const
{
    return (kouki_wake_verify_state_t)m_state.load(std::memory_order_acquire);
}

void WakeVerify::reset()
{
    /* Задачу не убиваем: она сама выставит результат и завершится. */
    if (m_task == nullptr) {
        m_state.store(KOUKI_WAKE_VERIFY_IDLE, std::memory_order_release);
        m_transcript[0] = '\0';
    }
}

const char *WakeVerify::last_transcript() const
{
    return m_transcript;
}

WakeVerify &wake_verify()
{
    static WakeVerify s_instance(config(), http());
    return s_instance;
}

}  // namespace kouki

// Тонкий C-фасад: тела — по одной строке, поведение и esp_err_t 1:1 с прежним
// app_wake_verify.c. Публичный app_wake_verify.h не менялся.
extern "C" {

esp_err_t kouki_wake_verify_start(const int16_t *pcm, int samples)
{
    return kouki::wake_verify().start(pcm, samples);
}

kouki_wake_verify_state_t kouki_wake_verify_state(void)
{
    return kouki::wake_verify().state();
}

void kouki_wake_verify_reset(void)
{
    kouki::wake_verify().reset();
}

const char *kouki_wake_verify_last_transcript(void)
{
    return kouki::wake_verify().last_transcript();
}

bool kouki_wake_verify_matches(const char *transcript)
{
    return kouki::WakeVerify::matches(transcript);
}

}  // extern "C"
