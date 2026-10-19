/* C5VRX-5 by Twotoz and the C5VRX contributors: ESP32-C5 analog 5.8 GHz FPV
 * receiver. https://github.com/Twotoz/C5VRX - https://twotoz.github.io/C5VRX/ */
#include "receiver.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "rf.h"

volatile const char *s_boot_stage = "start";

void app_main(void)
{
    /* USB output never blocks a task when no host is attached. */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    fcntl(fileno(stdout), F_SETFL, fcntl(fileno(stdout), F_GETFL, 0) | O_NONBLOCK);
    usb_serial_jtag_vfs_use_nonblocking();
    /* The console first: flashing (download key) and diagnostics must stay
     * reachable whatever a later boot step does. */
    console_start();
    s_boot_stage = "rf";
    ESP_ERROR_CHECK(rf_start());
    s_boot_stage = "settings";
    settings_load();
    /* Start at the first index of the highest RF stage; Direct Gain V5 takes
     * over at once (table maximum without a carrier). */
    receiver_set_gain(arc_gain_highest_rf_stage_start(rf_gain_table()));
    s_boot_stage = "calibration";
    calibration_start();
    /* The receive profile (filter class + tracker) of this channel is
     * chosen before live TX starts; it never changes under live video. */
    profile_load();
    profile_boot();
    s_boot_stage = "transport";
    if (transport_start() != ESP_OK) {
        s_boot_stage = "transport_fault";
        printf("TRANSPORT boot failed: live OFF; USB status/download remain available\n");
        return;
    }
    s_boot_stage = "tasks";
    receiver_start();
    control_start();
    s_boot_stage = "ready";
    printf("C5VRX5 %s ready: %s %u MHz, U85Q, ultrafine IQ, Direct Gain V5 ('h' for keys)\n",
           esp_app_get_description()->version, rf_channel()->name, rf_frequency_mhz());
}
