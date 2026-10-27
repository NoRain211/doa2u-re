#ifndef DOA2U_RECOMP_SYNCTEST_H
#define DOA2U_RECOMP_SYNCTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One piece of host or guest state copied at a frame boundary. save writes
   size bytes; restore reads them back. */
typedef struct RecompSyncTestModule {
    const char *name;
    size_t size;
    void (*save)(void *state);
    void (*restore)(const void *state);
    /* Optional comparison; NULL means memcmp. */
    bool (*equal)(const void *left, const void *right, size_t size);
    /* Optional: false when this saved state cannot be rewound faithfully. */
    bool (*restorable)(const void *saved, size_t size);
    /* Host-scheduled state that is not part of the simulation. */
    bool ignore;
} RecompSyncTestModule;

typedef enum RecompSyncTestResult {
    RECOMP_SYNCTEST_RUNNING,
    RECOMP_SYNCTEST_MATCH,
    RECOMP_SYNCTEST_MISMATCH,
    RECOMP_SYNCTEST_SKIP,
} RecompSyncTestResult;

void recomp_synctest_set_modules(const RecompSyncTestModule *modules, size_t count);
/* The runtime's real state table; defined beside the runtime, not the core. */
void recomp_synctest_install_default_modules(void);
/* Extra state the runner owns (its scripted input schedule). */
void recomp_synctest_set_extra_state(
    size_t size, void (*save)(void *), void (*restore)(const void *));

/* One saved frame, for callers outside the check loop (run-ahead). */
typedef struct RecompSyncTestState RecompSyncTestState;
RecompSyncTestState *recomp_synctest_state_create(void);
void recomp_synctest_state_destroy(RecompSyncTestState *state);
bool recomp_synctest_state_save(RecompSyncTestState *state);
bool recomp_synctest_state_load(const RecompSyncTestState *state);
bool recomp_synctest_state_restorable(const RecompSyncTestState *state);

/* RECOMP_SYNCTEST=1 checks one frame, =loop keeps checking, and
   RECOMP_SYNCTEST_FRAMES sets the replay length. */
bool recomp_synctest_enabled(void);
bool recomp_synctest_loop(void);
void recomp_synctest_configure(bool enabled, unsigned frames, bool loop);
/* Call at each Swap after the frame-locked clock advances. */
RecompSyncTestResult recomp_synctest_frame(uint32_t swap_counter);

#ifdef __cplusplus
}
#endif

#endif
