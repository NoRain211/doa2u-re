#include "synctest.h"
#include "d3d8_adapter.h"
#include "xapi_time_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

enum { MAX_MODULES = 16u };

struct RecompSyncTestState {
    void *block[MAX_MODULES];
    void *extra;
    /* Guest caller of the Present that ended the frame. A frame that presents
       from another routine leaves the host in a different native continuation,
       which a rewind cannot restore. */
    uint32_t swap_caller;
};
typedef struct RecompSyncTestState Snapshot;

typedef enum Phase { PHASE_IDLE, PHASE_COUNT, PHASE_REPLAY, PHASE_DONE } Phase;

static RecompSyncTestModule modules[MAX_MODULES];
static size_t module_count;
static size_t extra_size;
static void (*extra_save)(void *);
static void (*extra_restore)(const void *);
static Snapshot baseline, reference, current;
static Phase phase;
static unsigned interval = 1u;
static uint32_t start_frame;
static unsigned countdown;
static bool enabled;
static bool loop_mode;
static bool allocated;
static bool env_read;
static int verbose = -1;
static double snapshot_ms, check_ms;
/* A fiber create/delete leaves the native contexts ahead of the saved state
   until the game settles into its new steady state. */
static unsigned settle_retries;
static bool from_settle;

enum { SETTLE_FRAMES = 60u, SETTLE_RETRIES = 3u };

static double now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    return 0.0;
#endif
}

static void read_env(void)
{
    if (env_read) return;
    env_read = true;
    if (verbose < 0) verbose = getenv("RECOMP_SYNCTEST_VERBOSE") != NULL;
    const char *value = getenv("RECOMP_SYNCTEST");
    if (value != NULL && *value != '\0' && strcmp(value, "0") != 0) {
        enabled = true;
        loop_mode = strcmp(value, "loop") == 0;
    }
    const char *frames = getenv("RECOMP_SYNCTEST_FRAMES");
    if (frames != NULL && *frames != '\0') {
        const unsigned parsed = (unsigned)strtoul(frames, NULL, 10);
        if (parsed != 0u) interval = parsed;
    }
    const char *start = getenv("RECOMP_SYNCTEST_START");
    if (start != NULL && *start != '\0') start_frame = (uint32_t)strtoul(start, NULL, 10);
}

bool recomp_synctest_enabled(void)
{
    read_env();
    return enabled;
}

bool recomp_synctest_loop(void)
{
    read_env();
    return loop_mode;
}

void recomp_synctest_configure(bool on, unsigned frames, bool loop)
{
    env_read = true;
    enabled = on;
    interval = frames == 0u ? 1u : frames;
    start_frame = 0u;
    loop_mode = loop;
    phase = PHASE_IDLE;
    settle_retries = 0u;
    from_settle = false;
}

void recomp_synctest_set_modules(const RecompSyncTestModule *table, size_t count)
{
    if (count > MAX_MODULES) count = MAX_MODULES;
    memcpy(modules, table, count * sizeof *table);
    module_count = count;
    allocated = false;
    phase = PHASE_IDLE;
}

void recomp_synctest_set_extra_state(
    size_t size, void (*save)(void *), void (*restore)(const void *))
{
    extra_size = size;
    extra_save = save;
    extra_restore = restore;
    allocated = false;
}

static void *allocate(size_t size)
{
    /* Zeroed: modules that save field by field leave struct padding unwritten. */
    void *block = calloc(1u, size == 0u ? 1u : size);
    if (block == NULL) {
        fprintf(stderr, "recomp synctest: out of memory for %zu bytes\n", size);
        exit(2);
    }
    return block;
}

static void allocate_snapshot(Snapshot *snapshot)
{
    for (size_t i = 0u; i < module_count; ++i)
        snapshot->block[i] = allocate(modules[i].size);
    snapshot->extra = extra_size != 0u ? allocate(extra_size) : NULL;
}

static void ensure_allocated(void)
{
    if (allocated) return;
    allocate_snapshot(&baseline);
    allocate_snapshot(&reference);
    allocate_snapshot(&current);
    allocated = true;
}

static void save_snapshot(Snapshot *snapshot)
{
    for (size_t i = 0u; i < module_count; ++i)
        modules[i].save(snapshot->block[i]);
    if (extra_save != NULL) extra_save(snapshot->extra);
    snapshot->swap_caller = recomp_d3d8_swap_caller();
}

static void load_snapshot(const Snapshot *snapshot)
{
    for (size_t i = 0u; i < module_count; ++i)
        modules[i].restore(snapshot->block[i]);
    if (extra_restore != NULL) extra_restore(snapshot->extra);
}

static bool snapshots_restorable(const Snapshot *snapshot)
{
    for (size_t i = 0u; i < module_count; ++i) {
        if (modules[i].restorable != NULL &&
            !modules[i].restorable(snapshot->block[i], modules[i].size)) {
            fprintf(stderr, "recomp synctest: %s cannot be rewound\n", modules[i].name);
            return false;
        }
    }
    return true;
}

RecompSyncTestState *recomp_synctest_state_create(void)
{
    if (module_count == 0u) return NULL;
    RecompSyncTestState *state = allocate(sizeof *state);
    memset(state, 0, sizeof *state);
    allocate_snapshot(state);
    return state;
}

void recomp_synctest_state_destroy(RecompSyncTestState *state)
{
    if (state == NULL) return;
    for (size_t i = 0u; i < module_count; ++i) free(state->block[i]);
    free(state->extra);
    free(state);
}

bool recomp_synctest_state_save(RecompSyncTestState *state)
{
    if (state == NULL) return false;
    save_snapshot(state);
    return true;
}

bool recomp_synctest_state_load(const RecompSyncTestState *state)
{
    if (state == NULL) return false;
    load_snapshot(state);
    return true;
}

bool recomp_synctest_state_restorable(const RecompSyncTestState *state)
{
    /* A frame presented from another routine leaves the host in a native
       continuation the rewind cannot resume (see swap_caller). */
    return state != NULL && state->swap_caller == recomp_d3d8_swap_caller() &&
        snapshots_restorable(state);
}

static bool compare_block(const RecompSyncTestModule *module, const void *left, const void *right)
{
    const char *name = module->name;
    const size_t size = module->size;
    if (module->equal != NULL && module->equal(left, right, size)) return true;
    const unsigned char *a = left, *b = right;
    size_t first = size, differing = 0u;
    for (size_t i = 0u; i < size; ++i) {
        if (a[i] == b[i]) continue;
        if (first == size) first = i;
        ++differing;
    }
    if (first == size) return true;
    fprintf(stderr, "recomp synctest: %s differs at +0x%zx (%zu bytes)\n",
        name, first, differing);
    if (verbose) {
        if (strcmp(name, "clock") == 0) {
            const RecompFrameClockState *a = left, *b = right;
            fprintf(stderr, "recomp synctest: clock left base=%llu offset=%llu frame=%llu enabled=%d\n",
                (unsigned long long)a->base_ns, (unsigned long long)a->offset_ns,
                (unsigned long long)a->frame, a->enabled);
            fprintf(stderr, "recomp synctest: clock right base=%llu offset=%llu frame=%llu enabled=%d\n",
                (unsigned long long)b->base_ns, (unsigned long long)b->offset_ns,
                (unsigned long long)b->frame, b->enabled);
        }
        const size_t start = first > 64u ? first - 64u : 0u;
        const size_t end = first + 96u < size ? first + 96u : size;
        for (size_t row = start; row < end; row += 16u) {
            fprintf(stderr, "recomp synctest: %s +0x%zx", name, row);
            for (size_t i = row; i < row + 16u && i < size; ++i)
                fprintf(stderr, " %02x/%02x", a[i], b[i]);
            fputc('\n', stderr);
        }
        if (strcmp(name, "guest_ram") == 0) {
            struct PageCount { size_t page; size_t count; } pages[16] = {{0}};
            size_t page_count = 0u;
            size_t printed = 0u;
            for (size_t i = 0u; i < size; ++i) {
                if (a[i] == b[i]) continue;
                if (printed < 16u) {
                    fprintf(stderr, "recomp synctest: guest_ram offset +0x%zx %02x/%02x\n",
                        i, a[i], b[i]);
                    ++printed;
                }
                const size_t page = i & ~(size_t)0xfffu;
                size_t slot = 0u;
                while (slot < page_count && pages[slot].page != page) ++slot;
                if (slot == page_count && page_count < 16u) {
                    pages[page_count].page = page;
                    pages[page_count].count = 0u;
                    ++page_count;
                }
                if (slot < 16u) ++pages[slot].count;
            }
            for (size_t i = 0u; i < page_count; ++i)
                fprintf(stderr, "recomp synctest: guest_ram page +0x%zx %zu bytes\n",
                    pages[i].page, pages[i].count);
        }
    }
    return false;
}

static bool compare_snapshots(const Snapshot *left, const Snapshot *right)
{
    bool match = true;
    for (size_t i = 0u; i < module_count; ++i) {
        if (modules[i].ignore) continue;
        if (!compare_block(&modules[i], left->block[i], right->block[i]))
            match = false;
    }
    if (extra_size != 0u &&
        !compare_block(&(RecompSyncTestModule){"runner_input", extra_size, NULL, NULL, NULL, NULL,
            false},
            left->extra, right->extra))
        match = false;
    return match;
}

RecompSyncTestResult recomp_synctest_frame(uint32_t swap_counter)
{
    read_env();
    if (!enabled || module_count == 0u) return RECOMP_SYNCTEST_RUNNING;
    ensure_allocated();
    switch (phase) {
    case PHASE_IDLE:
        if (swap_counter < start_frame) return RECOMP_SYNCTEST_RUNNING;
        from_settle = settle_retries != 0u;
        save_snapshot(&baseline);
        fprintf(stderr, "recomp synctest: saved frame %u; replaying %u frame(s)\n",
            swap_counter, interval);
        phase = PHASE_COUNT;
        countdown = interval;
        return RECOMP_SYNCTEST_RUNNING;
    case PHASE_COUNT:
        if (--countdown != 0u) return RECOMP_SYNCTEST_RUNNING;
        const double snapshot_start = now_ms();
        save_snapshot(&reference);
        const bool moved = reference.swap_caller != baseline.swap_caller;
        if (moved) {
            fprintf(stderr, "recomp synctest: frame %u skip: Present returned to %08x, "
                "rewind resumes in %08x\n", swap_counter,
                (unsigned)reference.swap_caller, (unsigned)baseline.swap_caller);
        }
        if (moved || !snapshots_restorable(&baseline)) {
            if (!moved)
                fprintf(stderr, "recomp synctest: frame %u skip: saved state cannot be rewound\n",
                    swap_counter);
            Snapshot swap = baseline;
            baseline = reference;
            reference = swap;
            if (!loop_mode) {
                phase = PHASE_DONE;
                return RECOMP_SYNCTEST_SKIP;
            }
            start_frame = swap_counter + SETTLE_FRAMES;
            if (settle_retries < SETTLE_RETRIES) ++settle_retries;
            phase = PHASE_IDLE;
            return RECOMP_SYNCTEST_RUNNING;
        }
        load_snapshot(&baseline);
        snapshot_ms = now_ms() - snapshot_start;
        phase = PHASE_REPLAY;
        countdown = interval;
        return RECOMP_SYNCTEST_RUNNING;
    case PHASE_REPLAY:
        if (--countdown != 0u) return RECOMP_SYNCTEST_RUNNING;
        const double check_start = now_ms();
        save_snapshot(&current);
        const bool match = compare_snapshots(&reference, &current);
        check_ms = now_ms() - check_start;
        if (match) {
            fprintf(stderr, "recomp synctest: frame %u match (snapshot %.1f ms, check %.1f ms)\n",
                swap_counter, snapshot_ms, check_ms);
            settle_retries = 0u;
            from_settle = false;
            if (loop_mode) {
                Snapshot swap = baseline;
                baseline = current;
                current = swap;
                phase = PHASE_COUNT;
                countdown = interval;
            } else {
                phase = PHASE_DONE;
            }
            return RECOMP_SYNCTEST_MATCH;
        }
        if (from_settle && settle_retries < SETTLE_RETRIES) {
            fprintf(stderr, "recomp synctest: frame %u unsettled after a fiber change; waiting\n",
                swap_counter);
            start_frame = swap_counter + SETTLE_FRAMES;
            ++settle_retries;
            phase = PHASE_IDLE;
            return RECOMP_SYNCTEST_RUNNING;
        }
        fprintf(stderr, "recomp synctest: frame %u MISMATCH\n", swap_counter);
        phase = PHASE_DONE;
        return RECOMP_SYNCTEST_MISMATCH;
    case PHASE_DONE:
    default:
        return RECOMP_SYNCTEST_RUNNING;
    }
}
