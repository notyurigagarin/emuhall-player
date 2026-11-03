#ifndef EMUHALL_ACHIEVEMENTS_H
#define EMUHALL_ACHIEVEMENTS_H
#include <stddef.h>
#include <stdint.h>

#define EH_RA_BADGE_SIZE 64
#define EH_RA_MAX_CHALLENGES 8

/* Presenter-thread snapshots: owned bytes, never pointers into the RA client.
   Pixels are top-down opaque BGRA32, ready for GDI StretchDIBits. */
typedef struct {
    unsigned id;
    char title[256];
    int badge_ready;
    uint32_t badge_pixels[EH_RA_BADGE_SIZE * EH_RA_BADGE_SIZE];
} eh_ra_challenge_t;

typedef struct {
    char heading[256], detail[256];
    unsigned unlocked, total, points, points_total;
    int kind; /* 1: game loaded, 2: achievement unlocked, 3: game completed */
    int hardcore, badge_ready;
    uint32_t badge_pixels[EH_RA_BADGE_SIZE * EH_RA_BADGE_SIZE];
} eh_ra_notification_t;

typedef struct {
    unsigned id;
    char title[256], progress[64];
    float percent;
    unsigned current, target;
    int has_counts, pinned, badge_ready;
    uint32_t badge_pixels[EH_RA_BADGE_SIZE * EH_RA_BADGE_SIZE];
} eh_ra_progress_t;

size_t eh_ra_challenges(eh_ra_challenge_t *out, size_t max);
int eh_ra_notification(eh_ra_notification_t *out);
int eh_ra_progress(eh_ra_progress_t *out);
void eh_ra_track(unsigned id);
#endif
