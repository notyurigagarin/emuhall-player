/* Copyright 2026 Braza IO Capital Holding. SPDX-License-Identifier: MIT */
/* Original synthetic queue traces, without a game, sound card or timing sleeps. */
#include "../../crates/emuhall-core-host/native/audio_pacing.h"
#include "../../crates/emuhall-core-host/native/timestretch.h"
#include <math.h>
#include <stdio.h>
static unsigned samples,crossings;static int16_t previous;
static void collect(int16_t left,int16_t right){(void)right;if(previous<0&&left>=0)crossings++;previous=left;samples++;}
int main(void){
    double tempo=1.0;
    /* A fast core refills to 90 ms; the player's wait drains it to 60 ms.
       Feeding that post-wait depth to the controller used to slow healthy play. */
    for(unsigned i=0;i<600;i++)tempo=eh_audio_sync_tempo(tempo,90.0,64.0,1.0);
    if(tempo!=1.0){puts("FAIL healthy producer was slowed");return 1;}
    double wrong=1.0;
    for(unsigned i=0;i<600;i++)wrong=eh_audio_sync_tempo(wrong,60.0,64.0,1.0);
    if(wrong>0.97){puts("FAIL fixture did not reproduce post-wait feedback bias");return 2;}
    for(unsigned i=0;i<300;i++)tempo=eh_audio_sync_tempo(tempo,32.0,64.0,1.25);
    if(tempo<0.69||tempo>0.71){puts("FAIL sustained shortage was ignored");return 3;}
    for(unsigned i=0;i<300;i++)tempo=eh_audio_sync_tempo(tempo,54.0,64.0,0.75);
    if(tempo!=1.0){puts("FAIL recovery did not return to normal speed");return 4;}
    tempo=eh_audio_sync_tempo(tempo,0.0,64.0,0.75);
    if(tempo<0.96||tempo>=1.0){puts("FAIL transient shortage was not smoothed");return 5;}
    eh_stretch_reset();
    for(unsigned i=0;i<48000;i++){int16_t tone=(int16_t)(12000*sin(i*6.283185307179586*440/48000));eh_stretch_push(tone,tone,0.8,collect);}
    if(samples<55000||samples>61000){puts("FAIL time stretch did not extend the audio duration");return 6;}
    double pitch=(double)crossings*48000/samples;
    if(pitch<430||pitch>450){puts("FAIL time stretch changed the test tone's pitch");return 7;}
    for(unsigned i=0;i<4800;i++)eh_stretch_push(1000,1000,1.0,collect);
    unsigned before=samples;for(unsigned i=0;i<4800;i++)eh_stretch_push(1000,1000,1.0,collect);
    if(samples-before!=4800){puts("FAIL time stretch did not return to direct playback");return 8;}
    puts("PASS healthy refill, feedback regression, sustained shortage, recovery and jitter");
    return 0;
}
