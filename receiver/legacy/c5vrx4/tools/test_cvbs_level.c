/* C5VRX by Twotoz and contributors. Check the lost-amplitude failure mode. */
#include "cvbs_level.h"
#include "cvbs_tables.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static c5v4_cvbs_stats_t signal(int span, int blank)
{
    c5v4_cvbs_stats_t v = {.levels_valid=true,.repeated=1,.pulses=2,.span_bins=span,
        .blank_bins=blank,.sync_bins=blank-span,.period_raw=2559};
    return v;
}
static void converge(c5v4_level_t *s, c5v4_cvbs_stats_t *v, uint32_t ctx)
{
    for (unsigned k=0;k<80;++k) {
        uint8_t before[256]; memcpy(before,s->codes,256);
        c5v4_level_observe(s,v,true,ctx,(uint64_t)(k+1)*100000);
        for(unsigned j=0;j<256;++j) {
            int difference=(int)c5v4_dac_uv[s->codes[j]]-c5v4_dac_uv[before[j]];
            assert(difference>=-32000 && difference<=32000);
        }
    }
}
static int mv(const c5v4_level_t *s,int d)
{
    /* no winding for the reference plateau samples */
    unsigned i=(unsigned)(d+128)/4;
    return (int)(c5v4_dac_uv[s->codes[i]]/1000);
}
static void check_seed_from_loaded_table(void)
{
    /* M may load LEGACY_FULL while c5v4_dac_codes is STD150. The servo must slew
     * from the loaded entries: its first update stays within 32 mV. */
    unsigned far = 0;
    for (unsigned i = 0; i < 256; ++i) {
        int d = (int)c5v4_dac_codes[i] - c5v4_dac_legacy_codes[i];
        if (d > 1 || d < -1) ++far;
    }
    assert(far); /* Seeding from the default table would jump. */
    c5v4_level_t s; c5v4_level_init(&s);
    c5v4_level_seed(&s, c5v4_dac_legacy_codes);
    assert(!memcmp(s.codes, c5v4_dac_legacy_codes, 256));
    c5v4_cvbs_stats_t v = signal(30, 4);
    bool changed = false;
    for (unsigned k = 0; k < 3u && !changed; ++k)
        changed = c5v4_level_observe(&s, &v, true, 1, (uint64_t)(k + 1) * 100000);
    assert(changed);
    for (unsigned i = 0; i < 256; ++i) {
        int d = (int)c5v4_dac_uv[s.codes[i]] - c5v4_dac_uv[c5v4_dac_legacy_codes[i]];
        assert(d >= -32000 && d <= 32000);
    }
}
static void check_fade_and_rate(void)
{
    c5v4_level_t s; c5v4_level_init(&s);
    c5v4_cvbs_stats_t v=signal(38,2);
    uint64_t now=0;
    for(unsigned j=0;j<40;++j,now+=20000) c5v4_level_observe(&s,&v,true,1,now);
    v=signal(18,2); /* sync phase separation shrinks by >50% */
    unsigned accepted=0;
    for(unsigned j=0;j<30;++j,now+=20000) {
        uint8_t before[256];memcpy(before,s.codes,256);
        accepted+=c5v4_level_observe(&s,&v,true,1,now);
        for(unsigned k=0;k<256;++k) {
            int d=(int)c5v4_dac_uv[s.codes[k]]-c5v4_dac_uv[before[k]];
            assert(d>=-32000&&d<=32000);
        }
    }
    assert(accepted && mv(&s,2)-mv(&s,-16)>=265);
    uint8_t held[256];memcpy(held,s.codes,256);
    /* Replayed/too-close samples cannot advance evidence or actuator. */
    assert(!c5v4_level_observe(&s,&v,true,1,now-19000));
    assert(!memcmp(held,s.codes,256));
    assert(!c5v4_level_observe(&s,&v,true,1,now+200000));
    assert(s.good==1); /* outage reacquires, no immediate table write */
    v.sync_mad_bins=4;v.span_bins=18;
    assert(!c5v4_level_observe(&s,&v,true,1,now+220000));
    assert(!memcmp(held,s.codes,256));
    v=signal(38,2);v.period_raw=2541; /* NTSC target */
    for(unsigned j=0;j<40;++j) c5v4_level_observe(&s,&v,true,2,now+300000+j*20000);
    assert(s.target_depth_mv==286);
    assert(mv(&s,2)-mv(&s,-36)>=250 && mv(&s,2)-mv(&s,-36)<=320);
}
static void check_gain_step(void)
{
    c5v4_level_t s; c5v4_level_init(&s);
    c5v4_cvbs_stats_t v=signal(40,2);
    uint64_t now=1000;
    for (unsigned k=0;k<80;++k,now+=20000) c5v4_level_observe(&s,&v,true,1,now);
    assert(c5v4_level_period(&s,1,now)==20000);
    /* A different RF tuple changes both recovered offset and separation.
     * New epoch requires three new windows, then reacquires within 75 ms. */
    v=signal(20,18);
    uint8_t held[256];memcpy(held,s.codes,256);
    assert(c5v4_level_period(&s,2,now)==5000);
    assert(!c5v4_level_observe(&s,&v,true,2,now));
    assert(!c5v4_level_observe(&s,&v,true,2,now+5000));
    assert(!memcmp(held,s.codes,256));
    for (unsigned k=2;k<=15;++k) c5v4_level_observe(&s,&v,true,2,now+k*5000);
    assert(mv(&s,18)>=285 && mv(&s,18)<=335);
    assert(mv(&s,-2)<=35);
    assert(mv(&s,18)-mv(&s,-2)>=270);
    assert(c5v4_level_period(&s,2,now+100000)==20000);
    /* A second switch cannot reuse old-context evidence or a replay, even
     * after an invalid/noise snapshot cleared qualification. */
    memcpy(held,s.codes,256);
    assert(!c5v4_level_observe(&s,&v,false,3,now+110000));
    assert(!c5v4_level_observe(&s,&v,true,3,now+70000));
    assert(s.good==0 && !memcmp(held,s.codes,256));
    assert(!c5v4_level_observe(&s,&v,true,3,now+115000));
    assert(s.good==1);
    v.sync_mad_bins=4;
    assert(!c5v4_level_observe(&s,&v,true,3,now+120000));
    assert(s.good==0 && !memcmp(held,s.codes,256));
}
static void check_electrical_slew(void)
{
    /* Every start/target, including a measured table in nonnumeric order.
     * DAC carry transitions must obey volts, and make progress toward goal. */
    uint32_t volts[64];
    for (unsigned c=0;c<64;++c) volts[c]=c5v4_dac_uv[(c*17)%64];
    for (unsigned from=0;from<64;++from) for (unsigned target=0;target<64;++target) {
        uint8_t code=from;
        for (unsigned k=0;k<64 && code!=target;++k) {
            uint8_t next=c5v4_level_slew(code,target,volts);
            int step=(int)volts[next]-(int)volts[code];
            int before=(int)volts[code]-(int)volts[target];
            int after=(int)volts[next]-(int)volts[target];
            if(before<0)before=-before;
            if(after<0)after=-after;
            assert(step>=-32000 && step<=32000);
            assert(after<before || before<=8000);
            code=next;
        }
        assert(code==target);
    }
}
/* HDZero: once settled, +-1-bin evidence jitter must not keep rewriting the
 * live LUT; a real ~4-bin (~30 mV) level change still corrects. */
static void check_settled_hold(void)
{
    c5v4_level_t s; c5v4_level_init(&s);
    c5v4_cvbs_stats_t v=signal(38,0);
    uint64_t now=1000000;
    for(unsigned k=0;k<200;++k,now+=20000) c5v4_level_observe(&s,&v,true,1,now);
    assert(s.settled);
    unsigned before=s.updates;
    for(unsigned k=0;k<250;++k,now+=20000) { /* 5 s of jittering evidence */
        v=signal(38+(int)(k%3)-1,(int)(k&1));
        c5v4_level_observe(&s,&v,true,1,now);
    }
    unsigned jitter_updates=s.updates-before;
    printf("settled hold: %u LUT updates in 5 s of +-1-bin jitter\n", jitter_updates);
    assert(jitter_updates<=1);
    int black=mv(&s,0);
    before=s.updates;
    v=signal(38,5);
    for(unsigned k=0;k<60;++k,now+=20000) c5v4_level_observe(&s,&v,true,1,now);
    assert(s.updates>before && mv(&s,5)>=285 && mv(&s,5)<=335 && mv(&s,0)<black);
}
int main(void)
{
    check_settled_hold();
    check_seed_from_loaded_table();
    check_fade_and_rate();
    check_gain_step();
    check_electrical_slew();
    for (int span=16;span<=80;span+=16) {
        for(int blank=-14;blank<=18;blank+=16) {
            c5v4_level_t s; c5v4_level_init(&s);
            c5v4_cvbs_stats_t v=signal(span,blank);
            assert(!c5v4_level_observe(&s,&v,true,1,0));
            assert(!c5v4_level_observe(&s,&v,true,1,100000));
            converge(&s,&v,1);
            assert(mv(&s,blank)>=285 && mv(&s,blank)<=335);
            assert(mv(&s,blank-span)<=35);
            /* Information-independent noise must not increase gain after loss. */
            uint8_t held[256]; memcpy(held,s.codes,256);
            for(unsigned k=0;k<100;++k) c5v4_level_observe(&s,&v,false,1,9000000+k*100000);
            assert(!memcmp(held,s.codes,256));
            v.levels_valid=false;
            assert(!c5v4_level_observe(&s,&v,true,1,20000000));
            assert(!memcmp(held,s.codes,256));
        }
    }
    c5v4_level_t s; c5v4_level_init(&s);
    c5v4_cvbs_stats_t v=signal(16,2);
    converge(&s,&v,1);
    uint8_t held[256];memcpy(held,s.codes,256);
    assert(!c5v4_level_observe(&s,&v,true,2,9000000)); /* new epoch, reacquire */
    assert(!memcmp(held,s.codes,256));
    v.span_bins=8; assert(!c5v4_level_observe(&s,&v,true,2,10000000));
    v=signal(16,2); v.origin_pm=900;
    assert(!c5v4_level_observe(&s,&v,true,2,11000000));
    assert(!memcmp(held,s.codes,256));
    for(unsigned upper=0;upper<1024;++upper) for(unsigned code=0;code<64;++code) {
        uint16_t word=c5v4_level_word((uint16_t)((upper<<6)|32),code);
        assert((word>>6)==upper && (word&63)==code);
    }
    puts("PASS CVBS servo: amplitude/CFO sweeps, convergence, electrical slew, fast gain-step recovery, loss hold, epoch and upper-bit preservation");
}
