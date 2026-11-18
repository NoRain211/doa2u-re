#include "rollback.h"

#include <string.h>

enum { HISTORY = RECOMP_ROLLBACK_HISTORY };

void recomp_rollback_init(RecompRollbackSession *s, uint32_t input_delay)
{
    memset(s, 0, sizeof *s);
    s->local_count = s->remote_count = input_delay;
    s->rewind_to = RECOMP_ROLLBACK_NONE;
    s->window = RECOMP_ROLLBACK_WINDOW;
}

/* A rewind reaches at most WINDOW frames back, so older history can be
   overwritten. */
static bool fits(const RecompRollbackSession *s, uint32_t frame)
{
    return frame + RECOMP_ROLLBACK_WINDOW < s->frame + HISTORY;
}

/* Zeroes what DOA2's pad update (0x000FA760) reads as zero, so noise there
   is no misprediction: sticks within 0x2000 of center, face, Black and White
   under 0x20, triggers under 0x80. Only that function reads the raw values. */
static RecompInputGamepad canonical(RecompInputGamepad pad)
{
    int16_t *thumbs[] = {&pad.thumb_lx, &pad.thumb_ly, &pad.thumb_rx, &pad.thumb_ry};
    for (size_t i = 0u; i < sizeof thumbs / sizeof *thumbs; ++i)
        if (*thumbs[i] > -0x2000 && *thumbs[i] < 0x2000) *thumbs[i] = 0;
    for (size_t i = 0u; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i)
        if (pad.analog_buttons[i] < (i < 6u ? 0x20u : 0x80u)) pad.analog_buttons[i] = 0u;
    return pad;
}

bool recomp_rollback_add_local(RecompRollbackSession *s, const RecompInputGamepad *pad)
{
    if (!fits(s, s->local_count)) return false;
    s->local[s->local_count++ % HISTORY] = canonical(*pad);
    return true;
}

bool recomp_rollback_add_remote(RecompRollbackSession *s, const RecompInputGamepad *pad)
{
    const uint32_t frame = s->remote_count;
    if (!fits(s, frame)) return false;
    if (frame < s->frame && memcmp(&s->used[frame % HISTORY], pad, sizeof *pad) != 0 &&
        (s->rewind_to == RECOMP_ROLLBACK_NONE || frame < s->rewind_to))
        s->rewind_to = frame;
    s->remote[frame % HISTORY] = *pad;
    ++s->remote_count;
    return true;
}

bool recomp_rollback_can_run(const RecompRollbackSession *s)
{
    return s->frame < s->local_count && s->frame < s->remote_count + s->window;
}

uint32_t recomp_rollback_take_rewind(RecompRollbackSession *s)
{
    const uint32_t frame = s->rewind_to;
    s->rewind_to = RECOMP_ROLLBACK_NONE;
    if (frame != RECOMP_ROLLBACK_NONE) s->frame = frame;
    return frame;
}

void recomp_rollback_run(RecompRollbackSession *s,
    RecompInputGamepad *local, RecompInputGamepad *remote)
{
    const uint32_t frame = s->frame++;
    *local = s->local[frame % HISTORY];
    if (frame < s->remote_count)
        *remote = s->remote[frame % HISTORY];
    else if (s->remote_count != 0u)
        *remote = s->remote[(s->remote_count - 1u) % HISTORY];
    else
        memset(remote, 0, sizeof *remote);
    s->used[frame % HISTORY] = *remote;
}
