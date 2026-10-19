/* C5VRX by Twotoz and contributors. Diagnostic only; no realtime writes. */
#include "cvbs_monitor.h"
#include "cvbs_tables.h"
#include <string.h>

static int abs_i(int x) { return x < 0 ? -x : x; }
static int clamp(int x,int lo,int hi) { return x<lo?lo:x>hi?hi:x; }
static int signed4(unsigned x) { return x<8?(int)x:(int)x-16; }
static unsigned signs(uint8_t r) { return ((r>>7)&1u)|(((r>>3)&1u)<<1); }
static int median(const int *v, size_t n)
{
    /* Windows here contain at most 48 points. Bounded insertion sort. */
    int a[48];
    if (!n || n>48) return 0;
    for (size_t i=0;i<n;i++) {
        a[i]=v[i];
        for (size_t j=i;j && a[j]<a[j-1];j--) {
            int t=a[j];a[j]=a[j-1];a[j-1]=t;
        }
    }
    return a[n/2];
}
static int mad(const int *v,size_t n,int centre)
{
    int a[48];
    if (n>48) return 999;
    for(size_t i=0;i<n;i++) a[i]=abs_i(v[i]-centre);
    return median(a,n);
}
static unsigned index_for(int p,int c,unsigned cls)
{
    unsigned e=(((128+c)&254)+((-p)&254))&255;
    return (e>>2)|(cls<<6);
}
static int delta_for(unsigned idx)
{
    int d=(int)(idx&63)*4+2-128;
    unsigned cls=idx>>6;
    if(cls==3) return 0;
    if(cls==1 && d<0) d+=256;
    if(cls==2 && d>=0) d-=256;
    return d;
}
static bool s_mask_decode;
void c5v4_cvbs_set_mask_decode(bool enabled) { s_mask_decode = enabled; }
const uint8_t *c5v4_cvbs_phase_table(bool mask) { return mask ? c5v4_phase_mask : c5v4_phase_static; }
static unsigned decode(uint8_t r,unsigned previous,bool history)
{
    if(!history) return (s_mask_decode ? c5v4_phase_mask : c5v4_phase_static)[r];
    return (((-previous)&255)>>7 ? c5v4_phase_history1 : c5v4_phase_history0)[r];
}

static bool low_at(const int16_t *v,size_t k,size_t count,int threshold)
{
    if(k<2 || k+2>=count) return false;
    unsigned low=0;
    for(size_t j=k-2;j<=k+2;j++) low+=v[j]<threshold;
    return low>=3;
}

void c5v4_cvbs_analyze(const uint8_t *raw,size_t n,bool history,unsigned transfer,
                       c5v4_cvbs_stats_t *out)
{
    memset(out,0,sizeof(*out));
    if(!raw || n<3000 || n>8190) return;
    /* C5 TX phase/stream alignment is not tagged in a frozen DMA snapshot.
     * Use one local stride-3 alignment and discard warmup. This is a semantic
     * estimator, not a claim of byte-exact output or measured connector volts. */
    /* 9.7 KiB, once: as a stack frame it was reserved again in every caller
     * task and left no DMA-capable heap for the menu. Not reentrant: the
     * firmware serializes callers (video.c cvbs_analyze_locked). */
    static struct { int16_t delta[2730]; uint8_t code[2730]; uint16_t hist[768]; } ws;
    int16_t *delta=ws.delta;
    uint8_t *code=ws.code;
    uint16_t *hist=ws.hist;
    memset(hist,0,sizeof(ws.hist));
    size_t count=0;
    unsigned previous=decode(raw[0],0,history), ambiguous=0,origin=0,clip=0;
    int sum_i=0,sum_q=0;
    for(size_t k=0;k<n;k+=3) {
        int i=signed4(raw[k]>>4),q=signed4(raw[k]&15);
        int ci=2*i+1,cq=2*q+1;
        origin+=(ci*ci+cq*cq)<=16;
        clip+=i==-8||i==7||q==-8||q==7;
        sum_i+=ci;sum_q+=cq;
    }
    unsigned sampled=(unsigned)((n+2)/3);
    out->origin_pm=origin*1000/sampled;out->clip_pm=clip*1000/sampled;
    out->mean_i_mcell=sum_i*500/(int)sampled;
    out->mean_q_mcell=sum_q*500/(int)sampled;
    for(size_t k=3;k<n;k+=3) {
        unsigned current=decode(raw[k],previous,history);
        unsigned trajectory=signs(raw[k-3])|(signs(raw[k-2])<<2)|
                            (signs(raw[k-1])<<4)|(signs(raw[k])<<6);
        unsigned cls=c5v4_trajectory[trajectory];
        unsigned idx=index_for((int)previous,(int)current,cls);
        previous=current;
        if(k<12) continue;
        int d=delta_for(idx);
        delta[count]=(int16_t)d;
        code[count]=(transfer==1u?c5v4_dac_legacy_codes:
                     transfer==2u?c5v4_dac_cvbs150_codes:c5v4_dac_codes)[idx];
        ++hist[d+384]; ambiguous+=cls==3; ++count;
    }
    out->pairs=(unsigned)count;out->ambiguous_pm=ambiguous*1000/(unsigned)count;
    /* Robust low/centre levels locate negative sync independently of the DAC
     * scale. A dark scene is valid; noise percentile alone is never lock. */
    int low=0,centre=0;unsigned cumulative=0;
    bool have_low=false;
    for(unsigned j=0;j<768;j++) {
        cumulative+=hist[j];
        if(!have_low && cumulative>=count*3/100) {low=(int)j-384;have_low=true;}
        if(cumulative>=count/2) {centre=(int)j-384;break;}
    }
    if(centre-low<12 || out->ambiguous_pm>250 || out->origin_pm>600) return;
    int threshold=(low+centre)/2;
    size_t previous_start=0;bool have_previous=false;
    for(size_t k=3;k+15<count;k++) {
        if(!low_at(delta,k,count,threshold) || low_at(delta,k-1,count,threshold)) continue;
        size_t end=k;
        while(end<count && low_at(delta,end,count,threshold)) ++end;
        size_t width=end-k;
        if(width<52 || width>76 || end+6>=count) {k=end;continue;}
        int pulse[48],blank[4],pulse_mv[48],blank_mv[4];
        size_t from=k+width/4,to=end-width/4,points=to-from;
        if(!points || points>48) {k=end;continue;}
        for(size_t j=0;j<points;j++) {
            pulse[j]=delta[from+j];pulse_mv[j]=(int)(c5v4_dac_uv[code[from+j]]/1000);
        }
        /* 0.15..0.375 us after sync: before NTSC/PAL burst onset. */
        for(size_t j=0;j<4;j++) {
            blank[j]=delta[end+2+j];blank_mv[j]=(int)(c5v4_dac_uv[code[end+2+j]]/1000);
        }
        int s=median(pulse,points),b=median(blank,4);
        int sm=mad(pulse,points,s),bm=mad(blank,4,b);
        if(b-s<12 || b-s>120 || sm>12 || bm>12) {k=end;continue;}
        ++out->pulses;
        if(have_previous) {
            size_t period=k-previous_start;
            /* NTSC ~847.4 and PAL ~853.3 unique samples/line at 13.333M. */
            if(period>=843 && period<=858) {
                ++out->repeated;out->period_raw=(unsigned)(period*3);
                out->sync_bins=s;out->blank_bins=b;out->span_bins=b-s;
                out->sync_mad_bins=sm;out->blank_mad_bins=bm;
                out->sync_mv=median(pulse_mv,points);
                out->blank_mv=median(blank_mv,4);
                out->sync_depth_mv=out->blank_mv-out->sync_mv;
                out->levels_valid=true; /* Valid phase plateaus even if fixed DAC mapping clips. */
            }
        }
        have_previous=true;previous_start=k;k=end;
    }
    /* Proposal only. Never an automatic actuator: a small measured phase
     * span can mean lost information, not merely the wrong output gain. */
    if(out->levels_valid && out->sync_depth_mv>0)
        out->suggested_scale_q10=(unsigned)clamp(300*1024/out->sync_depth_mv,768,1536);
}
