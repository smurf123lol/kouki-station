#pragma once

/*
 * Заглушка esp_netif.h для host-стенда Wifi (T121 П7).
 *
 * Сетевой интерфейс подменён: esp_netif_get_ip_info отдаёт адрес, заложенный
 * тестом через wifi_mock_set_ip(). Событие получения адреса приходит из
 * esp_netif_types.h в IDF — здесь те же типы объявлены рядом, чтобы
 * app_wifi.cpp собирался без IDF.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* В IDF esp_netif_t — неполный тип, а функции возвращают указатель на него. */
typedef struct esp_netif_obj esp_netif_t;

typedef struct {
    uint32_t addr;
} esp_ip4_addr_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

/** База события получения адреса (в IDF — esp_event base). */
extern const char *IP_EVENT;
#define IP_EVENT_STA_GOT_IP 0

typedef struct {
    esp_netif_ip_info_t ip_info;
    int32_t ip_changed;
} ip_event_got_ip_t;

#define esp_ip4_addr1_16(a) ((int)((a)->addr & 0xffu))
#define esp_ip4_addr2_16(a) ((int)(((a)->addr >> 8) & 0xffu))
#define esp_ip4_addr3_16(a) ((int)(((a)->addr >> 16) & 0xffu))
#define esp_ip4_addr4_16(a) ((int)(((a)->addr >> 24) & 0xffu))
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) esp_ip4_addr1_16(a), esp_ip4_addr2_16(a), esp_ip4_addr3_16(a), esp_ip4_addr4_16(a)

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_create_default_wifi_sta(void);
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *ifkey);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip_info);
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *addr, char *buf, int buflen);

#ifdef __cplusplus
}
#endif
