#include "synctest.h"

#include "d3d8_adapter.h"
#include "d3d_vblank.h"
#include "dsound_api_adapter.h"
#include "dsound_buffer_adapter.h"
#include "dsound_effects_adapter.h"
#include "fiber_adapter.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "program_manual.h"
#include "runtime.h"
#include "save_transaction.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Audio worker counters and their copies on worker stacks advance with host
   scheduling, not with the frame. Rollback excludes audio the same way. */
static const struct { uint32_t offset, size; } ram_ignore[] = {
    {0xf2ad60u, 0x50u}, {0xf3e800u, 0x10u}, {0xf5cde0u, 0x20u}, {0x1056c80u, 0xa0u},
    /* QPC bookkeeping: frame start/elapsed/fps and the task scheduler's last
       QPC. These are host-time observations, not simulation state. */
    {0xdf3790u, 0x30u}, {0xa94810u, 0x60u},
};

typedef RecompRamRange IgnoreRange;

static size_t build_ignore_ranges(IgnoreRange *ranges, size_t capacity)
{
    size_t count = 0u;
    for (size_t i = 0u; i < sizeof ram_ignore / sizeof ram_ignore[0] && count < capacity; ++i)
        ranges[count++] = (IgnoreRange){ram_ignore[i].offset, ram_ignore[i].offset + ram_ignore[i].size};
    RecompFiberStackRange stacks[RECOMP_FIBER_MAX_COUNT];
    const size_t stack_count = recomp_fiber_adapter_stack_ranges(stacks, RECOMP_FIBER_MAX_COUNT);
    for (size_t i = 0u; i < stack_count && count < capacity; ++i)
        ranges[count++] = (IgnoreRange){stacks[i].base, (size_t)stacks[i].base + stacks[i].size};
    /* Worker threads park on their own stacks, which hold host-scheduled
       scratch. Their heap allocations and TLS blocks are compared. */
    RecompGuestStackRange thread_stacks[32];
    const size_t thread_stack_count = recomp_kernel_thread_stack_ranges(thread_stacks, 32u);
    /* These report every stack; one past its array makes the list fail below as a full one. */
    for (size_t i = 0u; i < thread_stack_count && i < 32u && count < capacity; ++i)
        ranges[count++] = (IgnoreRange){thread_stacks[i].base,
            (size_t)thread_stacks[i].base + thread_stacks[i].size};
    RecompGuestStackRange kernel_stacks[16];
    const size_t kernel_stack_count = recomp_kernel_memory_stack_ranges(kernel_stacks, 16u);
    for (size_t i = 0u; i < kernel_stack_count && i < 16u && count < capacity; ++i)
        ranges[count++] = (IgnoreRange){kernel_stacks[i].base,
            (size_t)kernel_stacks[i].base + kernel_stacks[i].size};
    if (getenv("RECOMP_SYNCTEST_VERBOSE") != NULL) {
        for (size_t i = 0u; i < stack_count; ++i)
            fprintf(stderr, "recomp synctest: fiber stack +0x%x..+0x%x\n",
                stacks[i].base, stacks[i].base + stacks[i].size);
        for (size_t i = 0u; i < thread_stack_count && i < 32u; ++i)
            fprintf(stderr, "recomp synctest: thread stack +0x%x..+0x%x\n",
                thread_stacks[i].base, thread_stacks[i].base + thread_stacks[i].size);
    }
    /* The reserved final MiB: startup stack, kernel data and the PCR page. */
    if (count < capacity) ranges[count++] = (IgnoreRange){0x03f00000u, 0x04000000u};
    const size_t wanted = sizeof ram_ignore / sizeof ram_ignore[0] + stack_count + thread_stack_count +
        kernel_stack_count + 1u;
    if (count < wanted) return SIZE_MAX;
    // Small list; insertion sort keeps the walk below simple.
    for (size_t i = 1u; i < count; ++i) {
        IgnoreRange value = ranges[i];
        size_t j = i;
        while (j > 0u && ranges[j - 1u].start > value.start) {
            ranges[j] = ranges[j - 1u];
            --j;
        }
        ranges[j] = value;
    }
    return count;
}

static size_t synctest_ignore_ranges(IgnoreRange ranges[64])
{
    const size_t count = build_ignore_ranges(ranges, 64u);
    if (count == SIZE_MAX) {
        fprintf(stderr, "recomp synctest: ignore range list full\n");
        exit(2);
    }
    return count;
}

static bool ram_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = left, *b = right;
    IgnoreRange ranges[64];
    const size_t range_count = synctest_ignore_ranges(ranges);
    size_t at = 0u;
    for (size_t range = 0u; range <= range_count && at < size; ++range) {
        size_t end = range < range_count && ranges[range].start < size ? ranges[range].start : size;
        if (end > at && memcmp(a + at, b + at, end - at) != 0) {
            if (getenv("RECOMP_SYNCTEST_VERBOSE") != NULL) {
                while (a[at] == b[at]) ++at;
                fprintf(stderr, "recomp synctest: ram_equal rejects +0x%zx\n", at);
            }
            return false;
        }
        if (range < range_count && ranges[range].stop > at) at = ranges[range].stop;
    }
    return true;
}

void recomp_synctest_mask_ignored(uint8_t *ram, size_t size)
{
    IgnoreRange ranges[64];
    const size_t range_count = synctest_ignore_ranges(ranges);
    for (size_t i = 0u; i < range_count; ++i) {
        if (ranges[i].start >= size) continue;
        const size_t stop = ranges[i].stop < size ? ranges[i].stop : size;
        memset(ram + ranges[i].start, 0, stop - ranges[i].start);
    }
}

size_t recomp_synctest_ignored_ranges(RecompRamRange *ranges, size_t capacity)
{
    return build_ignore_ranges(ranges, capacity);
}

/* Guest time plus the presenter marks derived from it; both rewind together
   or a replayed frame waits on a vblank from the abandoned timeline. */
typedef struct ClockState {
    RecompFrameClockState clock;
    uint64_t vblank[2];
} ClockState;
static size_t clock_size(void) { return sizeof(ClockState); }
static void clock_save(void *state)
{
    ClockState *out = state;
    recomp_xapi_frame_clock_save(&out->clock);
    recomp_d3d_vblank_state_save(out->vblank);
}
static void clock_restore(const void *state)
{
    const ClockState *in = state;
    recomp_xapi_frame_clock_restore(&in->clock);
    recomp_d3d_vblank_state_restore(in->vblank);
}
static size_t heap_size(void) { return sizeof(XboxHeapState); }
static void heap_save(void *state) { xbox_HeapStateSave(state); }
static void heap_restore(const void *state) { xbox_HeapStateRestore(state); }
static bool fibers_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_fiber_adapter_state_restorable(state);
}
static bool thread_fibers_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_kernel_thread_fiber_state_restorable(state);
}
static bool threads_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_kernel_thread_state_restorable(state);
}
static bool effects_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_dsound_effects_state_restorable(state);
}
/* Guest RAM from before a file close still holds the handle the host closed. */
static bool file_positions_restorable(const void *state, size_t size)
{
    (void)size;
    return recomp_kernel_file_state_restorable(state);
}
/* Save storage and its journal live on the host disk, outside the snapshot.
   A replay after a save sees the files the first pass wrote and takes another
   branch, and a rewind into an open transaction would replay its begin and
   never commit. Neither kind of frame is rewound. */
typedef struct SaveStorageState {
    bool pending;
    uint32_t mutations;
} SaveStorageState;
static void save_storage_save(void *state)
{
    *(SaveStorageState *)state = (SaveStorageState){
        recomp_save_pending(), recomp_kernel_profile_mutations()};
}
static void save_storage_restore(const void *state) { (void)state; }
static bool save_storage_restorable(const void *state, size_t size)
{
    const SaveStorageState *saved = state;
    (void)size;
    return !saved->pending && saved->mutations == recomp_kernel_profile_mutations();
}

/* A host read worker may still be writing guest RAM for a parked thread, and
   that thread resumes inside the read. Neither can be rewound. */
static void host_reads_save(void *state) { *(unsigned *)state = recomp_kernel_host_waits(); }
static void host_reads_restore(const void *state) { (void)state; }
static bool host_reads_restorable(const void *state, size_t size)
{
    (void)size;
    return *(const unsigned *)state == 0u && recomp_kernel_host_waits() == 0u;
}

void recomp_synctest_install_default_modules(void)
{
    RecompSyncTestModule table[15];
    size_t count = 0u;
    recomp_synctest_set_ram_equal(ram_equal);
    table[count++] = (RecompSyncTestModule){"cpu", recomp_runtime_state_size(),
        recomp_runtime_state_save, recomp_runtime_state_restore,
        recomp_runtime_state_equal, NULL, false};
    table[count++] = (RecompSyncTestModule){"clock", clock_size(), clock_save, clock_restore,
        NULL, NULL, true};
    table[count++] = (RecompSyncTestModule){"fibers", recomp_fiber_adapter_state_size(),
        recomp_fiber_adapter_state_save, recomp_fiber_adapter_state_restore,
        recomp_fiber_adapter_state_equal,
        fibers_restorable, false};
    table[count++] = (RecompSyncTestModule){"threads", recomp_kernel_thread_state_size(),
        recomp_kernel_thread_state_save, recomp_kernel_thread_state_restore, NULL,
        threads_restorable, true};
    table[count++] = (RecompSyncTestModule){"thread_fibers",
        recomp_kernel_thread_fiber_state_size(),
        recomp_kernel_thread_fiber_state_save, recomp_kernel_thread_fiber_state_restore,
        NULL, thread_fibers_restorable, true};
    table[count++] = (RecompSyncTestModule){"file_positions",
        recomp_kernel_file_state_size(),
        recomp_kernel_file_state_save, recomp_kernel_file_state_restore,
        NULL, file_positions_restorable, false};
    table[count++] = (RecompSyncTestModule){"allocations", recomp_kernel_memory_state_size(),
        recomp_kernel_memory_state_save, recomp_kernel_memory_state_restore};
    table[count++] = (RecompSyncTestModule){"heap", heap_size(), heap_save, heap_restore};
    table[count++] = (RecompSyncTestModule){"input", recomp_input_adapter_state_size(),
        recomp_input_adapter_state_save, recomp_input_adapter_state_restore};
    table[count++] = (RecompSyncTestModule){"d3d8", recomp_d3d8_state_size(),
        recomp_d3d8_state_save, recomp_d3d8_state_restore};
    table[count++] = (RecompSyncTestModule){"dsound_api", recomp_dsound_api_state_size(),
        recomp_dsound_api_state_save, recomp_dsound_api_state_restore};
    table[count++] = (RecompSyncTestModule){"dsound_buffers", recomp_dsound_buffer_state_size(),
        recomp_dsound_buffer_state_save, recomp_dsound_buffer_state_restore};
    table[count++] = (RecompSyncTestModule){"dsound_effects", recomp_dsound_effects_state_size(),
        recomp_dsound_effects_state_save, recomp_dsound_effects_state_restore, NULL,
        effects_restorable};
    table[count++] = (RecompSyncTestModule){"save_storage", sizeof(SaveStorageState),
        save_storage_save, save_storage_restore, NULL, save_storage_restorable, true};
    table[count++] = (RecompSyncTestModule){"host_reads", sizeof(unsigned),
        host_reads_save, host_reads_restore, NULL, host_reads_restorable, true};
    recomp_synctest_set_modules(table, count);
}

void recomp_reset_runtime_modules(void)
{
    recomp_kernel_file_reset();
    /* Before the memory reset: these free their guest pool allocations. */
    recomp_dsound_api_reset();
    recomp_d3d8_reset_keeping_window();
    recomp_kernel_thread_reset();
    recomp_fiber_adapter_reset();
    recomp_kernel_memory_reset();
    recomp_kernel_interrupt_reset();
    recomp_kernel_video_reset();
    xbox_HeapReset();
    recomp_program_manual_reset();
    recomp_input_adapter_reset();
    recomp_xapi_frame_clock_reset();
}
