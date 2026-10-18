/* Optional home Wi-Fi for the C5VRX Wi-Fi bridge: besides its own access
 * point the S3 joins a home network (AP+STA), so any computer on that
 * network can open the page. The network name and password are entered on
 * the bridge's page and stored in the S3's NVS only. 2.4 GHz networks only;
 * while joined, the bridge's own network moves to the home network's channel. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum { HOMEWIFI_OFF = 0, HOMEWIFI_CONNECTING, HOMEWIFI_CONNECTED } homewifi_state_t;

/* After esp_netif_init(), the default event loop and esp_wifi_init():
 * creates the station interface and loads the stored network. */
void homewifi_init(void);
/* True when a home network is stored (start Wi-Fi in AP+STA mode). */
bool homewifi_configured(void);
/* After esp_wifi_set_mode(): hands the stored network to the driver. */
void homewifi_apply(void);
/* Stores and joins a network now; an empty ssid forgets it (AP only). */
esp_err_t homewifi_set(const char *ssid, const char *password);
homewifi_state_t homewifi_state(void);
const char *homewifi_state_name(homewifi_state_t s);
/* Why joining fails, in plain words ("" when joined or unknown). */
const char *homewifi_hint(void);
/* Copies the stored network name and the address on it ("" if none). */
void homewifi_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len);
void homewifi_print(void);
