#include "demod_quality.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(demod_phase5_signed_delta(0, 4) == 4);
    assert(demod_phase5_signed_delta(31, 1) == 2);
    assert(demod_phase5_signed_delta(1, 31) == -2);

    /* Equivalent to a phase trajectory that crosses the principal branch:
     * adjacent steps are +10,+10 while the endpoint wraps to -12. */
    assert(demod_phase5_endpoint_loses_winding(0, 10, 20));

    /* Ordinary local trajectory is represented identically by endpoint and
     * adjacent interpretations. */
    assert(!demod_phase5_endpoint_loses_winding(0, 4, 8));
    assert(!demod_phase5_endpoint_loses_winding(30, 31, 0));

    assert(demod_winding_penalty(83) == 8);
    assert(!demod_static_heavy(179));
    assert(demod_static_heavy(180));

    puts("Demod quality: Phase5 adjacent-winding oracle passed");
}
