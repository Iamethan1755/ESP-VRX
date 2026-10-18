/* Optional home Wi-Fi (AP+STA), see homewifi.h. */
#include "homewifi.h"
#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#define NVS_NS "bridge"

static const char *TAG = "homewifi";
static esp_netif_t *s_netif;
static char s_ssid[33], s_pass[65];
static volatile homewifi_state_t s_state;
static char s_ip[16];
static int s_failures, s_last_reason;
static esp_timer_handle_t s_retry;

static void load(void)
{
    nvs_handle_t h;
    s_ssid[0] = s_pass[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof s_ssid;
    if (nvs_get_str(h, "sta_ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = 0;
    n = sizeof s_pass;
    if (nvs_get_str(h, "sta_pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (s_ssid[0]) {
        err = nvs_set_str(h, "sta_ssid", s_ssid);
        if (err == ESP_OK) err = nvs_set_str(h, "sta_pass", s_pass);
    } else {
        nvs_erase_key(h, "sta_ssid");
        nvs_erase_key(h, "sta_pass");
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Plain-words meaning of a disconnect reason. */
static const char *reason_hint(int reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found: name right, and a 2.4 GHz network?";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "network found but its security does not match: password missing, or a WPA3/Enterprise-only network?";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "wrong password?";
    default:
        return "";
    }
}

static void connect_now(void *arg)
{
    (void)arg;
    if (s_ssid[0]) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_ssid[0]) {
            s_state = HOMEWIFI_CONNECTING;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        s_ip[0] = 0;
        if (!s_ssid[0]) {
            s_state = HOMEWIFI_OFF;
            return;
        }
        s_state = HOMEWIFI_CONNECTING;
        s_last_reason = e->reason;
        s_failures++;
        /* Searching for the home network takes the radio off the bridge's
         * channel: retry quickly a few times, then only every 30 s. */
        const uint64_t delay_us = s_failures <= 5 ? 1000000ull : 30000000ull;
        if (s_failures == 1 || s_failures == 6 || s_failures % 20 == 0)
            printf("HOMEWIFI \"%s\" not joined (reason %d %s), retrying every %llu s\n", s_ssid, e->reason,
                   reason_hint(e->reason), (unsigned long long)(delay_us / 1000000));
        esp_timer_stop(s_retry);
        esp_timer_start_once(s_retry, delay_us);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_state = HOMEWIFI_CONNECTED;
        s_failures = 0;
        printf("HOMEWIFI joined \"%s\": open http://%s/ (or http://c5vrx-bridge.local/)\n", s_ssid, s_ip);
    }
}

void homewifi_init(void)
{
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "c5vrx-bridge");
    const esp_timer_create_args_t retry = {.callback = connect_now, .name = "homewifi"};
    esp_timer_create(&retry, &s_retry);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, on_event, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    load();
}

bool homewifi_configured(void) { return s_ssid[0] != 0; }

void homewifi_apply(void)
{
    if (!s_ssid[0]) return;
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, s_pass, sizeof sta.sta.password);
    sta.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;   /* WPA3 routers too */
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    ESP_LOGI(TAG, "joining \"%s\"", s_ssid);
}

esp_err_t homewifi_set(const char *ssid, const char *password)
{
    if (strlen(ssid) > 32 || strlen(password) > 63 || (password[0] && strlen(password) < 8))
        return ESP_ERR_INVALID_ARG;
    /* An empty password for the network already stored keeps its password:
     * pressing Join again must not wipe it. */
    const bool keep = !password[0] && ssid[0] && strcmp(ssid, s_ssid) == 0;
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    if (!keep) strlcpy(s_pass, password, sizeof s_pass);
    esp_err_t err = save();
    s_failures = 0;
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    if (!s_ssid[0]) {
        s_state = HOMEWIFI_OFF;
        s_ip[0] = 0;
        esp_wifi_set_mode(WIFI_MODE_AP);
        printf("HOMEWIFI forgotten: bridge network only\n");
        return err;
    }
    s_state = HOMEWIFI_CONNECTING;
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    homewifi_apply();
    if (mode != WIFI_MODE_APSTA) esp_wifi_set_mode(WIFI_MODE_APSTA);   /* STA_START then connects */
    else esp_wifi_connect();
    printf("HOMEWIFI joining \"%s\"\n", s_ssid);
    return err;
}

homewifi_state_t homewifi_state(void) { return s_state; }

const char *homewifi_hint(void)
{
    return s_ssid[0] && s_state != HOMEWIFI_CONNECTED ? reason_hint(s_last_reason) : "";
}

const char *homewifi_state_name(homewifi_state_t s)
{
    return s == HOMEWIFI_CONNECTED ? "connected" : s == HOMEWIFI_CONNECTING ? "connecting" : "off";
}

void homewifi_info(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    strlcpy(ssid, s_ssid, ssid_len);
    strlcpy(ip, s_ip, ip_len);
}

void homewifi_print(void)
{
    if (!s_ssid[0]) {
        printf("HOMEWIFI off (set it on the bridge page)\n");
        return;
    }
    wifi_ap_record_t ap;
    const bool up = s_state == HOMEWIFI_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    printf("HOMEWIFI %s \"%s\" ip=%s rssi=%d channel=%d password=%s last_reason=%d %s\n",
           homewifi_state_name(s_state), s_ssid, s_ip[0] ? s_ip : "-", up ? ap.rssi : 0, up ? ap.primary : 0,
           s_pass[0] ? "set" : "EMPTY", s_last_reason, s_state == HOMEWIFI_CONNECTED ? "" : reason_hint(s_last_reason));
}
