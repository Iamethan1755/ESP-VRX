/* SPDX-License-Identifier: GPL-3.0-only */
/* Stored picture settings, see settings.h. */
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_SPACE   "bridge"
#define NVS_KEY     "picture"
#define LAYOUT      1      /* stored first; other layouts are ignored */
#define QUIET_MS    2000   /* written this long after the last change */

static const char *TAG = "settings";

typedef struct {
    uint8_t layout;
    settings_t s;
} stored_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static settings_t s_pending, s_saved;
static bool s_have_saved;           /* s_saved is what the flash holds */
static volatile bool s_erase;
static TaskHandle_t s_task;

static bool valid(const settings_t *s)
{
    return s->h_tenths >= -50 && s->h_tenths <= 50 && s->v_lines >= -20 && s->v_lines <= 20 && s->levels <= 1 &&
           s->filter <= 1 && s->colour <= 1 && s->denoise <= 1;
}

bool settings_load(settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_SPACE, NVS_READONLY, &h) != ESP_OK) return false;   /* nothing stored yet */
    stored_t st;
    size_t n = sizeof st;
    const bool ok = nvs_get_blob(h, NVS_KEY, &st, &n) == ESP_OK && n == sizeof st && st.layout == LAYOUT && valid(&st.s);
    nvs_close(h);
    if (!ok) return false;
    *s = st.s;
    portENTER_CRITICAL(&s_mux);
    s_saved = s_pending = st.s;
    s_have_saved = true;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

void settings_save(const settings_t *s)
{
    if (!valid(s)) return;
    portENTER_CRITICAL(&s_mux);
    s_pending = *s;
    portEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void settings_erase(void)
{
    s_erase = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void write_pending(void)
{
    portENTER_CRITICAL(&s_mux);
    const stored_t st = {.layout = LAYOUT, .s = s_pending};
    const bool erase = s_erase, same = s_have_saved && memcmp(&s_pending, &s_saved, sizeof s_saved) == 0;
    s_erase = false;
    portEXIT_CRITICAL(&s_mux);
    if (!erase && same) return;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_SPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (erase) {
            err = nvs_erase_key(h, NVS_KEY);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        } else {
            err = nvs_set_blob(h, NVS_KEY, &st, sizeof st);
        }
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "not %s: %s", erase ? "erased" : "saved", esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_saved = st.s;
    s_have_saved = !erase;
    portEXIT_CRITICAL(&s_mux);
    printf(erase ? "SETTINGS erased: defaults at the next start\n" : "SETTINGS saved\n");
}

static void settings_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(QUIET_MS))) {
            /* another change: wait again, so a row of taps is one write */
        }
        write_pending();
    }
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    /* The lowest priority on core 0: a save may wait for everything else. */
    if (xTaskCreatePinnedToCore(settings_task, "settings", 4096, NULL, 1, &s_task, 0) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
