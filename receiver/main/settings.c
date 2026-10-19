/* C5VRX-5 by Twotoz and the C5VRX contributors: persisted settings (NVS
 * namespace c5vrx5, separate from older generations). */
#include "receiver.h"
#include <stdio.h>
#include "nvs.h"
#include "rf.h"

#define NAMESPACE "c5vrx5"
#define SETTINGS_VERSION 1u

typedef struct {
    uint8_t version, channel, std_mode, reserved;
} settings_t;

bool blob_load(const char *key, void *data, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t length = size;
    esp_err_t err = nvs_get_blob(h, key, data, &length);
    nvs_close(h);
    return err == ESP_OK && length == size;
}

bool blob_store(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, data, size);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) printf("NVS store key=%s err=%s\n", key, esp_err_to_name(err));
    return err == ESP_OK;
}

void settings_save(void)
{
    settings_t s = {SETTINGS_VERSION, (uint8_t)rf_channel_index(), (uint8_t)s_std_mode, 0};
    settings_t old;
    if (blob_load("settings", &old, sizeof(old)) && old.version == s.version &&
        old.channel == s.channel && old.std_mode == s.std_mode) return;   /* flash wear */
    (void)blob_store("settings", &s, sizeof(s));
}

void settings_load(void)
{
    settings_t s;
    if (!blob_load("settings", &s, sizeof(s)) || s.version != SETTINGS_VERSION) return;
    if (s.channel < FPV_CHANNELS) (void)rf_set_channel(s.channel);
    if (s.std_mode <= STD_MODE_PAL) s_std_mode = (std_mode_t)s.std_mode;
}
