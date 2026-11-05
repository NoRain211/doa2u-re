#ifndef DOA2U_RECOMP_SFD_MOVIE_H
#define DOA2U_RECOMP_SFD_MOVIE_H

#include <stdbool.h>
#include <stdint.h>

/* Plays a Sofdec SFD on a host thread: MPEG-1 video through pl_mpeg, ADX
   audio to the given host output slot. Callers pass the playback clock in
   seconds; all calls come from one thread. */
typedef struct RecompSfdMovie RecompSfdMovie;

typedef enum RecompSfdState {
    RECOMP_SFD_PREPARING, RECOMP_SFD_PLAYING, RECOMP_SFD_ENDED, RECOMP_SFD_FAILED
} RecompSfdState;

/* NULL when the file cannot be opened. With audio false the movie is silent. */
RecompSfdMovie *recomp_sfd_movie_open(const char *host_path, uint32_t audio_slot, bool audio);
void recomp_sfd_movie_close(RecompSfdMovie *movie);
/* The first PLAYING report starts the clock at now; video frames are due from there. */
RecompSfdState recomp_sfd_movie_poll(RecompSfdMovie *movie, double now);
/* Takes the newest decoded frame due at now that has not been taken yet.
   Pixels are BGRA with opaque alpha, width * 4 bytes per row. */
bool recomp_sfd_movie_take_frame(RecompSfdMovie *movie, double now,
    const uint8_t **pixels, uint32_t *width, uint32_t *height);
void recomp_sfd_movie_set_volume(RecompSfdMovie *movie, int32_t hundredth_db);
/* Waits until the decoder can run no further ahead, so the answers that
   follow depend on the caller's clock alone (netplay and replays). */
void recomp_sfd_movie_settle(RecompSfdMovie *movie);
/* Copies the subtitle shown at now on channel into text; returns its length or 0. */
uint32_t recomp_sfd_movie_subtitle(RecompSfdMovie *movie, uint32_t channel, double now,
    char *text, uint32_t size);

#endif

