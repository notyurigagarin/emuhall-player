/* Pitch-preserving time-stretch for the player's 48 kHz output (timestretch.c). Portable C, no dependencies. */
#ifndef EMUHALL_TIMESTRETCH_H
#define EMUHALL_TIMESTRETCH_H
#include <stdint.h>

/* Pushes one stereo sample. tempo 1 passes samples straight through; below 1 (emulation slower than real time) the
   sound is stretched to fill the time at the same pitch. Each output sample goes to out. */
void eh_stretch_push(int16_t left,int16_t right,double tempo,void (*out)(int16_t,int16_t));
/* Drops buffered sound, e.g. after a state load or a reset of the sound device. */
void eh_stretch_reset(void);
#endif
