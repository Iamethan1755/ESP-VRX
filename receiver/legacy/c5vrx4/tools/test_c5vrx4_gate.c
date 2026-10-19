/* C5VRX by Twotoz and contributors: native gate must not undo analog LOCK. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include "driver/gptimer.h"
#include "nvs_flash.h"
#include "freertos/semphr.h"
static bool native;
static unsigned timer_calls;
static int saved_level = -1;
static uint64_t alarm_at;
static StaticSemaphore_t mutex_storage;
#define xSemaphoreCreateRecursiveMutex() xSemaphoreCreateRecursiveMutexStatic(&mutex_storage)
#define ESP_ERR_NO_MEM 4
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)
bool rf_native_agc_active(void) { return native; }
esp_err_t gptimer_new_timer(const gptimer_config_t *c, gptimer_handle_t *t)
{ assert(c->resolution_hz==1000000); ++timer_calls; *t=&timer_calls; return ESP_OK; }
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t t,const gptimer_event_callbacks_t *c,void *ctx)
{ (void)t; (void)ctx; assert(c->on_alarm); ++timer_calls; return ESP_OK; }
esp_err_t gptimer_enable(gptimer_handle_t t) { (void)t; ++timer_calls; return ESP_OK; }
esp_err_t gptimer_start(gptimer_handle_t t) { (void)t; ++timer_calls; return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t t) { (void)t; ++timer_calls; return ESP_OK; }
esp_err_t gptimer_set_raw_count(gptimer_handle_t t,uint64_t v)
{ (void)t; assert(v==0); ++timer_calls; return ESP_OK; }
esp_err_t gptimer_set_alarm_action(gptimer_handle_t t,const gptimer_alarm_config_t *a)
{ (void)t; alarm_at=a->alarm_count; ++timer_calls; return ESP_OK; }
uint8_t rf_get_iq_lanes(void) { return 2; }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h)
{ assert(!strcmp(name,"c5vrx4")); (void)mode; *h=1; return ESP_OK; }
static int saved_bw_code = -1;
static uint16_t saved_bw_width;
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v)
{
    (void)h;
    if (!strcmp(key,"level_lab") && saved_level >= 0) { *v=saved_level; return ESP_OK; }
    if (!strcmp(key,"bw_code") && saved_bw_code >= 0) { *v=(uint8_t)saved_bw_code; return ESP_OK; }
    return ESP_FAIL;
}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v)
{ (void)h; if (!strcmp(key,"bw_code")) { saved_bw_code=v; return ESP_OK; } return ESP_FAIL; }
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *v)
{ (void)h; if (!strcmp(key,"bw_width") && saved_bw_width) { *v=saved_bw_width; return ESP_OK; } return ESP_FAIL; }
esp_err_t nvs_set_u16(nvs_handle_t h,const char *key,uint16_t v)
{ (void)h; if (!strcmp(key,"bw_width")) { saved_bw_width=v; return ESP_OK; } return ESP_FAIL; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *v,size_t *n)
{ (void)h; (void)key; (void)v; (void)n; return ESP_FAIL; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *v,size_t n)
{ (void)h; (void)key; (void)v; (void)n; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "host"; }
void esp_restart(void) { assert(!"unexpected reboot"); }
void vTaskDelay(unsigned ticks) { (void)ticks; }
#include "../pipeline.c"
int main(void)
{
    /* Fixed analog BW: on by default, uncalibrated until a measurement is stored. */
    assert(c5vrx4_fixed_bw_enabled());
    assert(c5vrx4_bw_code() == C5VRX4_BW_UNCALIBRATED && c5vrx4_bw_target_khz() == 24000u);
    assert(c5vrx4_bw_store(52, 24375) && c5vrx4_bw_code() == 52 && c5vrx4_bw_width_khz() == 24375u);
    assert(saved_bw_code == 52 && saved_bw_width == 24375u);
    s_bw_loaded = false; saved_bw_code = 64; /* out-of-range code is ignored */
    assert(c5vrx4_bw_code() == C5VRX4_BW_UNCALIBRATED);
    assert(c5vrx4_cvbs_mode() == C5VRX4_CVBS_STD150);
    assert(!strcmp(c5vrx4_cvbs_mode_name(), "STD150"));
    assert(c5vrx4_level_enabled()); /* absent NVS enables the new default */
    saved_level=0;s_level_loaded=false;assert(!c5vrx4_level_enabled());
    saved_level=1;s_level_loaded=false;assert(c5vrx4_level_enabled());
    void *m=mmap((void *)0x600A0000,0x10000,PROT_READ|PROT_WRITE,
                 MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    assert(m!=MAP_FAILED);
    memset(m,0xA5,0x10000);
    unsigned char before[0x10000]; memcpy(before,m,sizeof(before));
    c5vrx4_start(); c5vrx4_suspend(); c5vrx4_suspend();
    c5vrx4_resume(); c5vrx4_resume();
    assert(c5vrx4_console('~')); assert(!c5vrx4_console('T'));
    /* Cover all shared lab keys: the hook passes them to the shared console. */
    const char *keys="{}[]HBWA:LlQ";
    for (const char *k=keys;*k;++k) assert(!c5vrx4_console(*k));
    assert(!timer_calls && !memcmp(before,m,sizeof(before)));
    native=true;
    c5vrx4_start(); assert(s_running && s_open && timer_calls);
    c5vrx4_suspend(); c5vrx4_suspend(); assert(!s_running);
    c5vrx4_resume(); assert(!s_running);
    c5vrx4_resume(); assert(s_running && s_open);
    gptimer_alarm_event_data_t e={.count_value=WINDOW_US,.alarm_value=WINDOW_US};
    gate_alarm(s_timer,&e,NULL);
    assert(!s_open && (AGC_CTRL&AGC_HOLD) && alarm_at==PERIOD_US);
    assert(c5vrx4_console('~') && !s_running && !(AGC_CTRL&AGC_HOLD));
    assert(c5vrx4_console('~') && s_running);
    c5vrx4_suspend();
    munmap(m,0x10000);
    puts("C5VRX-4 Direct Gain LOCK / native gate isolation passed");
}

void c5vrx4_lane_print(void) {}
