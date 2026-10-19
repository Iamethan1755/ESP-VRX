/* C5VRX by Twotoz and contributors: stale strong input must not cut range. */
#include <assert.h>
#include <stdio.h>
#include "rx_control_epoch.h"
#include "direct_gain_v3.h"
unsigned char phy_param[0x800];
int main(void)
{
    arc_gain_table_t table;
    arc_gain_table_from_bytes(&table, NULL, 81u);
    direct_gain_v3_t dg;
    direct_gain_v3_reset(&dg, &table, table.max_index, 62u);
    dg3_observation_t overload={.p50=60,.p95=110,.clip_pm=300,
                               .coherence=90,.observed_us=10000};
    rx_control_epoch_t captured={1,2,3};
    rx_control_epoch_t changed[]={{2,2,3},{1,3,3},{1,2,4}};
    for (unsigned n=0;n<3;++n) {
        if (rx_control_observation_current(captured,changed[n],10000,10200))
            (void)direct_gain_v3_tick(&dg,&overload);
        assert(dg.current_gain==table.max_index && dg.writes==0);
    }
    assert(!rx_control_observation_current(captured,captured,10000,9999));
    assert(!rx_control_observation_current(captured,captured,10000,11001));
    assert(rx_control_observation_current(captured,captured,10000,11000));
    if (rx_control_observation_current(captured,captured,10000,10200))
        (void)direct_gain_v3_tick(&dg,&overload);
    assert(dg.current_gain<table.max_index && dg.writes==1 && dg.overloads==1);
    puts("RX control epoch tests passed");
}
