/* Copyright 2026 Braza IO Capital Holding. SPDX-License-Identifier: MIT */
#ifndef EMUHALL_AUDIO_PACING_H
#define EMUHALL_AUDIO_PACING_H
/* Measure supply before the player deliberately waits for the queue to drain.
   A healthy producer must not be mistaken for a slow core after that wait. */
static inline double eh_audio_sync_tempo(double current,double produced_queue_ms,double target_ms,double work_ratio){
    double want=1.0+0.6*(produced_queue_ms-target_ms)/target_ms;
    /* With measured spare time, ordinary queue jitter must not hold a fast
       core at a permanently reduced tempo. Keep half a buffer as a guard;
       real shortages still use the gradual pitch-preserving slowdown. */
    if(work_ratio<0.9&&produced_queue_ms>target_ms*0.5)want=1.0;
    if(want>1.0)want=1.0;
    if(want<0.6)want=0.6;
    current+=(want-current)*(want<current?0.08:0.03);
    return current>0.998?1.0:current;
}
#endif
