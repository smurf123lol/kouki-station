/*
 * Мок NVS для host-стенда конфигурации (T121 П2).
 *
 * Заменяет nvs_flash: «флеш» — статические массивы в памяти процесса, API
 * повторяет IDF 5.5.1 (см. support/config_mock/nvs.h). Ошибки открытия и
 * чтения строк можно заказать заранее, чтобы проверить ветки модуля.
 */
#include "nvs_mock.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define MOCK_MAX_NAMESPACES 8
#define MOCK_MAX_KEYS 24
#define MOCK_MAX_VALUE_LEN 8300
#define MOCK_MAX_LOG_ENTRIES 256

typedef enum {
    MOCK_EMPTY = 0,
    MOCK_U8,
    MOCK_U32,
    MOCK_STR,
} mock_type_t;

typedef struct {
    char key[16];
    mock_type_t type;
    uint8_t u8;
    uint32_t u32;
    char str[MOCK_MAX_VALUE_LEN];
} mock_entry_t;

typedef struct {
    bool used;
    char name[16];
    nvs_open_mode_t mode;
    int count;
    mock_entry_t entries[MOCK_MAX_KEYS];
} mock_ns_t;

typedef struct {
    bool used;
    char ns[16];
    char key[16];
} mock_write_t;

static mock_ns_t s_namespaces[MOCK_MAX_NAMESPACES];
static mock_write_t s_writes[MOCK_MAX_LOG_ENTRIES];
static int s_write_count;
static char s_write_log[4096];
static char s_last_open_ns[16];
static esp_err_t s_pending_open_error;
static char s_pending_get_str_key[16];
static esp_err_t s_pending_get_str_error;
/* Однократный отказ записи/фиксации: нужен, чтобы проверить ветку
 * «save() вернул ошибку → портал отвечает 500 Ошибка записи в NVS». */
static esp_err_t s_pending_set_str_error;
static char s_pending_set_str_key[16];
static esp_err_t s_pending_commit_error;

static mock_ns_t *find_ns(const char *name)
{
    for (int i = 0; i < MOCK_MAX_NAMESPACES; i++) {
        if (s_namespaces[i].used && strncmp(s_namespaces[i].name, name, sizeof(s_namespaces[i].name) - 1) == 0) {
            return &s_namespaces[i];
        }
    }
    return NULL;
}

static mock_entry_t *find_entry(mock_ns_t *ns, const char *key)
{
    if (ns == NULL) {
        return NULL;
    }
    for (int i = 0; i < MOCK_MAX_KEYS; i++) {
        if (ns->entries[i].type != MOCK_EMPTY && strncmp(ns->entries[i].key, key, sizeof(ns->entries[i].key) - 1) == 0) {
            return &ns->entries[i];
        }
    }
    return NULL;
}

static mock_entry_t *upsert_entry(mock_ns_t *ns, const char *key, mock_type_t type)
{
    if (ns == NULL) {
        return NULL;
    }
    mock_entry_t *entry = find_entry(ns, key);
    if (entry == NULL) {
        for (int i = 0; i < MOCK_MAX_KEYS; i++) {
            if (ns->entries[i].type == MOCK_EMPTY) {
                entry = &ns->entries[i];
                entry->type = type;
                strncpy(entry->key, key, sizeof(entry->key) - 1);
                ns->count++;
                break;
            }
        }
    }
    if (entry != NULL) {
        entry->type = type;
        entry->u8 = 0;
        entry->u32 = 0;
        entry->str[0] = '\0';
    }
    return entry;
}

static void log_write(const char *ns, const char *key)
{
    for (int i = 0; i < MOCK_MAX_LOG_ENTRIES; i++) {
        if (s_writes[i].used && strcmp(s_writes[i].ns, ns) == 0 && strcmp(s_writes[i].key, key) == 0) {
            return;
        }
    }
    for (int i = 0; i < MOCK_MAX_LOG_ENTRIES; i++) {
        if (!s_writes[i].used) {
            s_writes[i].used = true;
            strncpy(s_writes[i].ns, ns, sizeof(s_writes[i].ns) - 1);
            strncpy(s_writes[i].key, key, sizeof(s_writes[i].key) - 1);
            s_write_count++;
            size_t used = strlen(s_write_log);
            snprintf(s_write_log + used, sizeof(s_write_log) - used, "%s%s:%s",
                     used > 0 ? "," : "", ns, key);
            return;
        }
    }
}

void nvs_mock_reset(void)
{
    memset(s_namespaces, 0, sizeof(s_namespaces));
    memset(s_writes, 0, sizeof(s_writes));
    s_write_count = 0;
    s_write_log[0] = '\0';
    s_last_open_ns[0] = '\0';
    s_pending_open_error = ESP_OK;
    s_pending_get_str_key[0] = '\0';
    s_pending_get_str_error = ESP_OK;
    s_pending_set_str_error = ESP_OK;
    s_pending_set_str_key[0] = '\0';
    s_pending_commit_error = ESP_OK;
}

void nvs_mock_fail_next_open(esp_err_t err)
{
    s_pending_open_error = err;
}

void nvs_mock_fail_next_set_str(const char *key, esp_err_t err)
{
    strncpy(s_pending_set_str_key, key, sizeof(s_pending_set_str_key) - 1);
    s_pending_set_str_error = err;
}

void nvs_mock_fail_next_commit(esp_err_t err)
{
    s_pending_commit_error = err;
}

void nvs_mock_fail_next_get_str(const char *key, esp_err_t err)
{
    strncpy(s_pending_get_str_key, key, sizeof(s_pending_get_str_key) - 1);
    s_pending_get_str_error = err;
}

bool nvs_mock_has_namespace(const char *ns)
{
    return find_ns(ns) != NULL;
}

int nvs_mock_count_keys(const char *ns)
{
    mock_ns_t *found = find_ns(ns);
    return found != NULL ? found->count : 0;
}

bool nvs_mock_has_key(const char *ns, const char *key)
{
    return find_entry(find_ns(ns), key) != NULL;
}

bool nvs_mock_get_u8(const char *ns, const char *key, uint8_t *out)
{
    mock_entry_t *entry = find_entry(find_ns(ns), key);
    if (entry == NULL || entry->type != MOCK_U8) {
        return false;
    }
    if (out != NULL) {
        *out = entry->u8;
    }
    return true;
}

bool nvs_mock_get_u32(const char *ns, const char *key, uint32_t *out)
{
    mock_entry_t *entry = find_entry(find_ns(ns), key);
    if (entry == NULL || entry->type != MOCK_U32) {
        return false;
    }
    if (out != NULL) {
        *out = entry->u32;
    }
    return true;
}

bool nvs_mock_get_str(const char *ns, const char *key, const char **out)
{
    mock_entry_t *entry = find_entry(find_ns(ns), key);
    if (entry == NULL || entry->type != MOCK_STR) {
        return false;
    }
    if (out != NULL) {
        *out = entry->str;
    }
    return true;
}

bool nvs_mock_was_written(const char *ns, const char *key)
{
    for (int i = 0; i < MOCK_MAX_LOG_ENTRIES; i++) {
        if (s_writes[i].used && strcmp(s_writes[i].ns, ns) == 0 && strcmp(s_writes[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

const char *nvs_mock_write_log(void)
{
    return s_write_log;
}

const char *nvs_mock_last_open_namespace(void)
{
    return s_last_open_ns;
}

void nvs_mock_seed_u8(const char *ns, const char *key, uint8_t value)
{
    mock_ns_t *target = find_ns(ns);
    if (target == NULL) {
        for (int i = 0; i < MOCK_MAX_NAMESPACES; i++) {
            if (!s_namespaces[i].used) {
                s_namespaces[i].used = true;
                strncpy(s_namespaces[i].name, ns, sizeof(s_namespaces[i].name) - 1);
                s_namespaces[i].mode = NVS_READWRITE;
                target = &s_namespaces[i];
                break;
            }
        }
    }
    mock_entry_t *entry = upsert_entry(target, key, MOCK_U8);
    if (entry != NULL) {
        entry->u8 = value;
    }
}

void nvs_mock_seed_u32(const char *ns, const char *key, uint32_t value)
{
    mock_ns_t *target = find_ns(ns);
    if (target == NULL) {
        for (int i = 0; i < MOCK_MAX_NAMESPACES; i++) {
            if (!s_namespaces[i].used) {
                s_namespaces[i].used = true;
                strncpy(s_namespaces[i].name, ns, sizeof(s_namespaces[i].name) - 1);
                s_namespaces[i].mode = NVS_READWRITE;
                target = &s_namespaces[i];
                break;
            }
        }
    }
    mock_entry_t *entry = upsert_entry(target, key, MOCK_U32);
    if (entry != NULL) {
        entry->u32 = value;
    }
}

void nvs_mock_seed_str(const char *ns, const char *key, const char *value)
{
    mock_ns_t *target = find_ns(ns);
    if (target == NULL) {
        for (int i = 0; i < MOCK_MAX_NAMESPACES; i++) {
            if (!s_namespaces[i].used) {
                s_namespaces[i].used = true;
                strncpy(s_namespaces[i].name, ns, sizeof(s_namespaces[i].name) - 1);
                s_namespaces[i].mode = NVS_READWRITE;
                target = &s_namespaces[i];
                break;
            }
        }
    }
    mock_entry_t *entry = upsert_entry(target, key, MOCK_STR);
    if (entry != NULL) {
        strncpy(entry->str, value, sizeof(entry->str) - 1);
    }
}

/* ----------------------------- NVS API ---------------------------------- */

static mock_ns_t *ns_from_handle(nvs_handle_t handle)
{
    if (handle == 0 || handle > MOCK_MAX_NAMESPACES) {
        return NULL;
    }
    mock_ns_t *ns = &s_namespaces[handle - 1];
    return ns->used ? ns : NULL;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    if (s_pending_open_error != ESP_OK) {
        esp_err_t err = s_pending_open_error;
        s_pending_open_error = ESP_OK;
        return err;
    }
    if (namespace_name == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_last_open_ns, namespace_name, sizeof(s_last_open_ns) - 1);

    mock_ns_t *ns = find_ns(namespace_name);
    if (ns == NULL) {
        if (open_mode == NVS_READONLY) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
        for (int i = 0; i < MOCK_MAX_NAMESPACES; i++) {
            if (!s_namespaces[i].used) {
                s_namespaces[i].used = true;
                strncpy(s_namespaces[i].name, namespace_name, sizeof(s_namespaces[i].name) - 1);
                ns = &s_namespaces[i];
                break;
            }
        }
        if (ns == NULL) {
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
    }
    ns->mode = open_mode;
    *out_handle = (nvs_handle_t)(ns - s_namespaces) + 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    mock_ns_t *ns = ns_from_handle(handle);
    mock_entry_t *entry = find_entry(ns, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != MOCK_U8) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (out_value != NULL) {
        *out_value = entry->u8;
    }
    return ESP_OK;
}

esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out_value)
{
    mock_ns_t *ns = ns_from_handle(handle);
    mock_entry_t *entry = find_entry(ns, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != MOCK_U32) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    if (out_value != NULL) {
        *out_value = entry->u32;
    }
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out_value, size_t *length)
{
    if (s_pending_get_str_error != ESP_OK && strcmp(s_pending_get_str_key, key) == 0) {
        esp_err_t err = s_pending_get_str_error;
        s_pending_get_str_error = ESP_OK;
        s_pending_get_str_key[0] = '\0';
        return err;
    }
    mock_ns_t *ns = ns_from_handle(handle);
    mock_entry_t *entry = find_entry(ns, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (entry->type != MOCK_STR) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }
    const size_t required = strlen(entry->str) + 1;
    if (length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_value == NULL) {
        *length = required;
        return ESP_OK;
    }
    if (*length < required) {
        *length = required;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->str, required);
    *length = required;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    mock_ns_t *ns = ns_from_handle(handle);
    if (ns == NULL) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    mock_entry_t *entry = upsert_entry(ns, key, MOCK_U8);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    entry->u8 = value;
    log_write(ns->name, key);
    return ESP_OK;
}

esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    mock_ns_t *ns = ns_from_handle(handle);
    if (ns == NULL) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    mock_entry_t *entry = upsert_entry(ns, key, MOCK_U32);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    entry->u32 = value;
    log_write(ns->name, key);
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    mock_ns_t *ns = ns_from_handle(handle);
    if (ns == NULL) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    if (s_pending_set_str_error != ESP_OK &&
        (s_pending_set_str_key[0] == '\0' || strcmp(s_pending_set_str_key, key) == 0)) {
        const esp_err_t err = s_pending_set_str_error;
        s_pending_set_str_error = ESP_OK;
        s_pending_set_str_key[0] = '\0';
        return err;
    }
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(value) >= MOCK_MAX_VALUE_LEN) {
        return ESP_ERR_NVS_VALUE_TOO_LONG;
    }
    mock_entry_t *entry = upsert_entry(ns, key, MOCK_STR);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    memcpy(entry->str, value, strlen(value) + 1);
    log_write(ns->name, key);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    mock_ns_t *ns = ns_from_handle(handle);
    mock_entry_t *entry = find_entry(ns, key);
    if (entry == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    entry->type = MOCK_EMPTY;
    entry->key[0] = '\0';
    entry->u8 = 0;
    entry->u32 = 0;
    entry->str[0] = '\0';
    ns->count--;
    log_write(ns->name, key);
    return ESP_OK;
}

esp_err_t nvs_erase_all(nvs_handle_t handle)
{
    mock_ns_t *ns = ns_from_handle(handle);
    if (ns == NULL) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    memset(ns->entries, 0, sizeof(ns->entries));
    ns->count = 0;
    log_write(ns->name, "*");
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (s_pending_commit_error != ESP_OK) {
        const esp_err_t err = s_pending_commit_error;
        s_pending_commit_error = ESP_OK;
        return err;
    }
    return ESP_OK;
}

/* Вспомогательное для тестов: сколько записей в журнале (для отчёта). */
int nvs_mock_write_count(void)
{
    return s_write_count;
}
