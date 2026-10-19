#include "range_control.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static uint8_t tick(range_control_t *c, bool sync, int p, int q,
                    int clip, int origin, int winding)
{
    return range_control_tick(c, sync, sync ? 90 : 0,
                              p, q, clip, origin, winding);
}

int main(void) {
    range_control_t c;

    /* Clean semantic video: hold forever, no gain hunting. */
    range_control_reset(&c, 40);
    for (int i=0;i<2000;i++)
        assert(tick(&c, i%3==0, 25, 75, 0, 10, 80)==40);
    assert(c.locked);

    /* Emergency clipping cut remains bounded. */
    range_control_reset(&c,62);
    assert(tick(&c,0,10,30,100,30,300)==62);
    assert(tick(&c,0,10,30,100,30,300)==58);
    assert(tick(&c,0,10,30,100,30,300)==58);
    assert(tick(&c,0,10,30,100,30,300)==54);

    /* Weak video trial rolls back when the settled result gets worse. */
    range_control_reset(&c,40);
    for(int i=0;i<20;i++) tick(&c,i%3==0,10,60,0,10,100);
    assert(c.trial && c.gain==42);
    for(int i=0;i<20;i++) tick(&c,0,10,20,0,100,400);
    assert(!c.trial && c.gain==40 && c.cooldown>=80);

    /* At the range edge, no-video recovery returns to high gain. */
    range_control_reset(&c,40);
    for(int i=0;i<1000;i++) {
        uint8_t g=tick(&c,0,10,20,0,700,300);
        assert(g>=40 && g<=62);
        assert(!c.locked);
    }
    assert(c.gain==62);

    /* Valid sync but severe endpoint winding is NOT called static-free.
     * The controller may make one normal trial, then accepts a large quality
     * improvement instead of blindly holding the original gain. */
    range_control_reset(&c,40);
    for(int i=0;i<20;i++) tick(&c,i%3==0,25,72,0,15,260);
    assert(c.trial && c.gain==38);
    for(int i=0;i<20;i++) tick(&c,i%3==0,25,76,0,10,70);
    assert(!c.trial && c.gain==38);

    range_control_reset(&c,24);
    assert(!c.trial && !c.cooldown && !c.failures && c.gain==24);

    puts("Range controller: semantic lock, winding-aware trials, rollback and high-gain recovery passed");
}
