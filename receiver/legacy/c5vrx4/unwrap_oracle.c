/* Issue #144: exhaustive discrete Phase8 trajectory proof. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
static int wrap(int x) { return ((x + 128) & 255) - 128; }
static int quadrant_step(int a, int b) {
    int d = (b - a) & 3;
    return d == 3 ? -1 : d;
}
static int classify(int p, int m1, int m2, int c) {
    int a=quadrant_step(p>>6,m1>>6);
    int b=quadrant_step(m1>>6,m2>>6);
    int d=quadrant_step(m2>>6,c>>6);
    if (a==2 || b==2 || d==2) return 3;
    int n=a+b+d;
    return n>=2 ? 1 : n<=-2 ? 2 : 0;
}
int main(void) {
    uint64_t cases=0, winding=0, ordinary=0, invalid=0;
    for (int p=0;p<256;p++) for(int d0=-63;d0<=63;d0++) {
        int m1=(p+d0)&255;
        for(int d1=-63;d1<=63;d1++) {
            int m2=(m1+d1)&255;
            for(int d2=-63;d2<=63;d2++) {
                int c=(m2+d2)&255, ref=d0+d1+d2;
                int cls=classify(p,m1,m2,c);
                /* Bit24 is zero in both counter operands. O6/O7 preserve
                   the removed endpoint parity: restore their sum. */
                int e=(((128+c)&254)+((-p)&254))&255;
                int coarse=e-128;
                if(cls==1 && coarse<0) coarse+=256;
                if(cls==2 && coarse>=0) coarse-=256;
                int result=coarse+(p&1)+(c&1);
                if(cls==3 || result!=ref) {
                    fprintf(stderr,"FAIL p=%d steps=%d,%d,%d class=%d result=%d ref=%d\n",
                            p,d0,d1,d2,cls,result,ref);
                    return 1;
                }
                int endpoint=wrap(c-p);
                if(ref==endpoint) ordinary++; else winding++;
                invalid += cls==3;
                cases++;
            }
        }
    }
    printf("PASS cases=%"PRIu64" ordinary=%"PRIu64" winding=%"PRIu64
           " wrong=0 ambiguous=%"PRIu64" bound=abs(step)<=63/256 turns\n",
           cases,ordinary,winding,invalid);
    return 0;
}
