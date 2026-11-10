/*
 * Мок Wi-Fi/сети для host-стенда Wifi (T121 П7), см. wifi_mock.h.
 *
 * Компилируется gcc как C (как и http_mock.c): IDF-заголовки стенда писались
 * под C, а C++-сборка app_wifi.cpp только подключает их.
 */
#include "wifi_mock.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"

/* Базы событий: в IDF это строковые константы (esp_event base). */
const char *WIFI_EVENT = "WIFI_EVENT";
const char *IP_EVENT = "IP_EVENT";

/* ------------------------------------------------------------- состояние -- */

typedef struct {
    esp_event_handler_t handler;
    void *arg;
    esp_event_base_t base;
    int32_t id;
} handler_t;

#define MAX_HANDLERS 8

static handler_t s_handlers[MAX_HANDLERS];
static int s_handler_count;

static bool s_fire_sta_start = true;
static bool s_auto_got_ip;
static uint32_t s_auto_ip;
static uint32_t s_auto_gw;

static esp_err_t s_fail_init;
static esp_err_t s_fail_timer_create;
static esp_err_t s_fail_inactive_time;

static int s_init_calls;
static int s_connect_calls;
static int s_start_calls;
static int s_set_mode_calls;
static int s_set_config_calls;
static int s_inactive_calls = -1;
static int s_netif_init_calls;
static int s_create_sta_calls;
static int s_get_ip_info_calls;
static int s_event_group_create_calls;

static char s_ssid[33];
static char s_password[65];
static wifi_auth_mode_t s_authmode = WIFI_AUTH_OPEN;
static wifi_interface_t s_config_if = WIFI_IF_STA;
static wifi_mode_t s_mode = WIFI_MODE_NULL;

static esp_timer_cb_t s_timer_cb;
static void *s_timer_arg;
static const char *s_timer_name;
static int s_timer_create_calls;
static int s_timer_start_calls;
static int s_timer_stop_calls;
static uint64_t s_last_timer_us;

static uint32_t s_ip_addr;
static bool s_ap_ok;
static int8_t s_ap_rssi;

/* Группа событий: просто маска битов. */
struct event_group_mock {
    EventBits_t bits;
};

static struct event_group_mock s_event_group;

/* ---------------------------------------------------------------- события -- */

static void fire(esp_event_base_t base, int32_t id, void *data)
{
    for (int i = 0; i < s_handler_count; i++) {
        if (s_handlers[i].base != base) {
            continue;
        }
        if (s_handlers[i].id != -1 && s_handlers[i].id != id) {
            continue;
        }
        if (s_handlers[i].handler != NULL) {
            s_handlers[i].handler(s_handlers[i].arg, base, id, data);
        }
    }
}

void wifi_mock_fire_event(esp_event_base_t base, int32_t id, void *data)
{
    fire(base, id, data);
}

void wifi_mock_fire_retry_timer(void)
{
    if (s_timer_cb != NULL) {
        s_timer_cb(s_timer_arg);
    }
}

/* ------------------------------------------------------------ esp_event -- */

esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t event_base, int32_t event_id,
                                              esp_event_handler_t event_handler, void *event_handler_arg,
                                              esp_event_handler_instance_t *context)
{
    if (s_handler_count >= MAX_HANDLERS) {
        return ESP_ERR_NO_MEM;
    }
    s_handlers[s_handler_count].base = event_base;
    s_handlers[s_handler_count].id = event_id;
    s_handlers[s_handler_count].handler = event_handler;
    s_handlers[s_handler_count].arg = event_handler_arg;
    s_handler_count++;
    if (context != NULL) {
        *context = NULL;
    }
    return ESP_OK;
}

int wifi_mock_handler_count(void)
{
    return s_handler_count;
}

/* -------------------------------------------------------------- esp_wifi -- */

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config;
    s_init_calls++;
    if (s_fail_init != ESP_OK) {
        const esp_err_t err = s_fail_init;
        s_fail_init = ESP_OK;
        return err;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    s_set_mode_calls++;
    s_mode = mode;
    return ESP_OK;
}

esp_err_t esp_wifi_set_config(wifi_interface_t interface, const wifi_config_t *config)
{
    s_set_config_calls++;
    s_config_if = interface;
    if (config != NULL) {
        strncpy(s_ssid, (const char *)config->sta.ssid, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
        strncpy(s_password, (const char *)config->sta.password, sizeof(s_password) - 1);
        s_password[sizeof(s_password) - 1] = '\0';
        s_authmode = config->sta.threshold.authmode;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    s_start_calls++;
    if (s_fire_sta_start) {
        fire(WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_connect(void)
{
    s_connect_calls++;
    if (s_auto_got_ip) {
        ip_event_got_ip_t ev = {0};
        ev.ip_info.ip.addr = s_auto_ip;
        ev.ip_info.gw.addr = s_auto_gw;
        fire(IP_EVENT, IP_EVENT_STA_GOT_IP, &ev);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_set_inactive_time(wifi_interface_t interface, uint16_t sec)
{
    (void)interface;
    s_inactive_calls = (int)sec;
    if (s_fail_inactive_time != ESP_OK) {
        const esp_err_t err = s_fail_inactive_time;
        s_fail_inactive_time = ESP_OK;
        return err;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    if (!s_ap_ok || ap_info == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(ap_info, 0, sizeof(*ap_info));
    ap_info->rssi = s_ap_rssi;
    return ESP_OK;
}

/* ------------------------------------------------------------- esp_netif -- */

esp_err_t esp_netif_init(void)
{
    s_netif_init_calls++;
    return ESP_OK;
}

esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    s_create_sta_calls++;
    return (esp_netif_t *)&s_netif_init_calls;
}

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *ifkey)
{
    if (ifkey != NULL && strcmp(ifkey, "WIFI_STA_DEF") == 0) {
        return (esp_netif_t *)&s_netif_init_calls;
    }
    return NULL;
}

esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip_info)
{
    (void)netif;
    s_get_ip_info_calls++;
    if (ip_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ip_info, 0, sizeof(*ip_info));
    ip_info->ip.addr = s_ip_addr;
    return ESP_OK;
}

char *esp_ip4addr_ntoa(const esp_ip4_addr_t *addr, char *buf, int buflen)
{
    if (addr == NULL || buf == NULL || buflen <= 0) {
        return NULL;
    }
    snprintf(buf, (size_t)buflen, IPSTR, IP2STR(addr));
    return buf;
}

/* ------------------------------------------------------------- esp_timer -- */

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out_handle)
{
    s_timer_create_calls++;
    if (s_fail_timer_create != ESP_OK) {
        const esp_err_t err = s_fail_timer_create;
        s_fail_timer_create = ESP_OK;
        return err;
    }
    s_timer_cb = args->callback;
    s_timer_arg = args->arg;
    s_timer_name = args->name;
    if (out_handle != NULL) {
        *out_handle = (esp_timer_handle_t)&s_timer_cb;
    }
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    (void)timer;
    s_timer_stop_calls++;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    (void)timer;
    s_timer_start_calls++;
    s_last_timer_us = timeout_us;
    return ESP_OK;
}

/* -------------------------------------------------------- event_groups --- */

EventGroupHandle_t xEventGroupCreate(void)
{
    s_event_group_create_calls++;
    s_event_group.bits = 0;
    return &s_event_group;
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t group, EventBits_t bits)
{
    if (group != NULL) {
        group->bits |= bits;
    }
    return group != NULL ? group->bits : 0;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t group, EventBits_t bits,
                                BaseType_t clear_on_exit, BaseType_t wait_for_all,
                                TickType_t ticks_to_wait)
{
    (void)clear_on_exit;
    (void)wait_for_all;
    (void)ticks_to_wait;
    if (group == NULL) {
        return 0;
    }
    return group->bits & bits;
}

/* ---------------------------------------------------------------- журнал -- */

void wifi_mock_reset(void)
{
    memset(s_handlers, 0, sizeof(s_handlers));
    s_handler_count = 0;
    s_fire_sta_start = true;
    s_auto_got_ip = false;
    s_auto_ip = 0;
    s_auto_gw = 0;
    s_fail_init = ESP_OK;
    s_fail_timer_create = ESP_OK;
    s_fail_inactive_time = ESP_OK;
    s_init_calls = 0;
    s_connect_calls = 0;
    s_start_calls = 0;
    s_set_mode_calls = 0;
    s_set_config_calls = 0;
    s_inactive_calls = -1;
    s_netif_init_calls = 0;
    s_create_sta_calls = 0;
    s_get_ip_info_calls = 0;
    s_event_group_create_calls = 0;
    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_password, 0, sizeof(s_password));
    s_authmode = WIFI_AUTH_OPEN;
    s_config_if = WIFI_IF_STA;
    s_mode = WIFI_MODE_NULL;
    s_timer_cb = NULL;
    s_timer_arg = NULL;
    s_timer_name = NULL;
    s_timer_create_calls = 0;
    s_timer_start_calls = 0;
    s_timer_stop_calls = 0;
    s_last_timer_us = 0;
    s_ip_addr = 0;
    s_ap_ok = false;
    s_ap_rssi = 0;
    s_event_group.bits = 0;
}

void wifi_mock_set_fire_sta_start(bool enabled)
{
    s_fire_sta_start = enabled;
}

void wifi_mock_set_auto_got_ip(bool enabled, uint32_t ip_addr, uint32_t gw_addr)
{
    s_auto_got_ip = enabled;
    s_auto_ip = ip_addr;
    s_auto_gw = gw_addr;
}

void wifi_mock_fail_next_init(esp_err_t err)
{
    s_fail_init = err;
}

void wifi_mock_fail_next_timer_create(esp_err_t err)
{
    s_fail_timer_create = err;
}

void wifi_mock_fail_next_inactive_time(esp_err_t err)
{
    s_fail_inactive_time = err;
}

int wifi_mock_init_calls(void) { return s_init_calls; }
int wifi_mock_connect_calls(void) { return s_connect_calls; }
int wifi_mock_start_calls(void) { return s_start_calls; }
int wifi_mock_set_mode_calls(void) { return s_set_mode_calls; }
int wifi_mock_set_config_calls(void) { return s_set_config_calls; }
int wifi_mock_netif_init_calls(void) { return s_netif_init_calls; }
int wifi_mock_create_sta_calls(void) { return s_create_sta_calls; }
int wifi_mock_get_ip_info_calls(void) { return s_get_ip_info_calls; }

const char *wifi_mock_ssid(void) { return s_ssid; }
const char *wifi_mock_password(void) { return s_password; }
wifi_auth_mode_t wifi_mock_authmode(void) { return s_authmode; }
wifi_interface_t wifi_mock_config_interface(void) { return s_config_if; }
wifi_mode_t wifi_mock_mode(void) { return s_mode; }
int wifi_mock_inactive_time(void) { return s_inactive_calls; }

int wifi_mock_timer_create_calls(void) { return s_timer_create_calls; }
int wifi_mock_timer_start_calls(void) { return s_timer_start_calls; }
int wifi_mock_timer_stop_calls(void) { return s_timer_stop_calls; }
uint64_t wifi_mock_last_timer_us(void) { return s_last_timer_us; }
const char *wifi_mock_timer_name(void) { return s_timer_name; }
void *wifi_mock_timer_arg(void) { return s_timer_arg; }

int wifi_mock_event_group_create_calls(void) { return s_event_group_create_calls; }

void wifi_mock_set_ip(uint32_t ip_addr)
{
    s_ip_addr = ip_addr;
}

void wifi_mock_set_ap(bool ok, int8_t rssi)
{
    s_ap_ok = ok;
    s_ap_rssi = rssi;
}
