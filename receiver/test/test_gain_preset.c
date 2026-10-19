/* C5VRX-5 by Twotoz and the C5VRX contributors: host test of the gain map
 * preset import (main/gain.c dg_import_preset, main/gain_map_preset.h).
 *
 *   cc -O2 -std=c11 -I../main test_gain_preset.c ../main/gain.c ../main/arc_phy.c -lm -o t && ./t
 */
#include <stdio.h>
#include <string.h>
#include "gain.h"
#include "gain_map_preset.h"

unsigned char phy_param[2048];
static unsigned s_fail, s_run;
static void check(const char *name, int ok) { ++s_run; if (!ok) { ++s_fail; printf("FAIL %s\n", name); } }

int main(void)
{
    arc_gain_table_t t;
    arc_gain_table_from_bytes(&t, NULL, ARC_5G_GAIN_MAX);
    direct_gain_t g;
    memset(&g, 0, sizeof(g));
    uint16_t preset[ARC_5G_GAIN_MAX + 1u] = {0};
    preset[60] = 500; preset[61] = 600; preset[70] = 2000;
    dg_reset(&g, &t, 60);
    check("imports with an anchor entry", dg_import_preset(&g, preset, ARC_5G_GAIN_MAX + 1u, ARC_5G_GAIN_MAX, 1));
    check("anchor keeps its scale", g.relative_power_q10[60] == 1024u);
    check("ratios kept", g.relative_power_q10[70] == 4096u && g.relative_power_q10[61] == 1228u);
    check("lowest confidence, high uncertainty", g.confidence[70] == 1u && g.uncertainty_pm[70] == 600u);
    dg_reset(&g, &t, 40);
    check("no anchor entry: nothing imported", !dg_import_preset(&g, preset, ARC_5G_GAIN_MAX + 1u, ARC_5G_GAIN_MAX, 1));
    dg_reset(&g, &t, 60);
    check("other table: refused", !dg_import_preset(&g, preset, ARC_5G_GAIN_MAX + 1u, ARC_5G_GAIN_MAX - 1u, 1));
    /* Own, confident data wins over the preset. */
    dg_reset(&g, &t, 60);
    g.relative_power_q10[70] = 3000u; g.confidence[70] = 5u;
    (void)dg_import_preset(&g, preset, ARC_5G_GAIN_MAX + 1u, ARC_5G_GAIN_MAX, 1);
    check("own data kept", g.relative_power_q10[70] == 3000u && g.confidence[70] == 5u);
    /* The shipped header is well formed (empty until board data exists). */
    check("shipped preset matches the 5 GHz table", GAIN_MAP_PRESET_MAX_INDEX == ARC_5G_GAIN_MAX);
    printf("%s: %u checks, %u failures\n", s_fail ? "FAIL" : "PASS", s_run, s_fail);
    return s_fail ? 1 : 0;
}
