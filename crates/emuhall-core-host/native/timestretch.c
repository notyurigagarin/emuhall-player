/* When emulation runs slower than real time, the game produces less sound than the sound card plays, and the card runs
   dry: choppy sound. Standalone emulators stretch the sound instead (PCSX2's TimeStretch sync), so a game at 90% speed
   sounds 10% slower but continuous. This is an original WSOLA (waveform-similarity overlap-add) stretcher:
   the stream is cut into 40 ms windows; each window starts where its first 8 ms best match the end of the previous
   window (searched over 15 ms), the two are cross-faded, and the input advances by tempo x the output, so the pitch is
   unchanged. It is idle (samples pass straight through) whenever emulation keeps up. */
#include "timestretch.h"
#include <math.h>
#include <string.h>

#define SEQ 1920     /* window, 40 ms at 48 kHz */
#define OVERLAP 384  /* cross-fade, 8 ms */
#define SEEK 720     /* search range, 15 ms */
#define CAPACITY 16384

static float input[CAPACITY][2];static unsigned count;
static float tail[OVERLAP][2];static int have_tail,active;
static double advance; /* fractional input still to skip */

static int16_t clamp16(float v){return v>32767.0f?32767:v<-32768.0f?-32768:(int16_t)v;}

void eh_stretch_reset(void){count=0;have_tail=0;active=0;advance=0;}

/* The offset in [0,SEEK) whose first OVERLAP samples best match the previous window's end (normalised correlation of
   the mono sums). A coarse pass every 4 samples, then the neighbourhood of the best. */
static unsigned best_offset(void){
    unsigned best=0;double best_score=-1e30;
    for(int pass=0;pass<2;pass++){
        unsigned from=pass?(best>4?best-4:0):0,to=pass?(best+5<SEEK?best+5:SEEK):SEEK,step=pass?1:4;
        for(unsigned k=from;k<to;k+=step){
            double dot=0,energy=1e-9;
            for(unsigned i=0;i<OVERLAP;i+=2){
                float a=tail[i][0]+tail[i][1],b=input[k+i][0]+input[k+i][1];
                dot+=a*b;energy+=b*b;
            }
            double score=dot/sqrt(energy);
            if(score>best_score){best_score=score;best=k;}
        }
    }
    return best;
}

static void consume(unsigned n){if(n>count)n=count;memmove(input,input+n,(size_t)(count-n)*sizeof(input[0]));count-=n;}

void eh_stretch_push(int16_t left,int16_t right,double tempo,void (*out)(int16_t,int16_t)){
    if(tempo>0.999&&!active){out(left,right);return;}
    if(tempo>0.999&&active){
        /* Keeping up again: finish the cross-fade and hand back what is buffered, then pass through. */
        unsigned n=count<OVERLAP?count:OVERLAP;
        for(unsigned i=0;i<n;i++){float w=have_tail?(float)i/OVERLAP:1.0f;out(clamp16(tail[i][0]*(1-w)+input[i][0]*w),clamp16(tail[i][1]*(1-w)+input[i][1]*w));}
        for(unsigned i=n;i<count;i++)out(clamp16(input[i][0]),clamp16(input[i][1]));
        eh_stretch_reset();out(left,right);return;
    }
    if(tempo<0.5)tempo=0.5;
    active=1;
    if(count<CAPACITY){input[count][0]=left;input[count][1]=right;count++;}
    while(count>=SEEK+SEQ){
        unsigned k=0;
        if(!have_tail){
            for(unsigned i=0;i<SEQ-OVERLAP;i++)out(clamp16(input[i][0]),clamp16(input[i][1]));
        }else{
            k=best_offset();
            for(unsigned i=0;i<OVERLAP;i++){
                float w=(float)i/OVERLAP;
                out(clamp16(tail[i][0]*(1-w)+input[k+i][0]*w),clamp16(tail[i][1]*(1-w)+input[k+i][1]*w));
            }
            for(unsigned i=OVERLAP;i<SEQ-OVERLAP;i++)out(clamp16(input[k+i][0]),clamp16(input[k+i][1]));
        }
        memcpy(tail,input+k+SEQ-OVERLAP,sizeof(tail));have_tail=1;
        /* SEQ-OVERLAP samples went out; the input moves on by tempo times that. */
        advance+=(SEQ-OVERLAP)*tempo;unsigned skip=(unsigned)advance;advance-=skip;
        consume(skip);
    }
}
