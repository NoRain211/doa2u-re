#include "synctest.h"

#include "d3d8_adapter.h"
#include "d3d_vblank.h"
#include "dsound_api_adapter.h"
#include "dsound_buffer_adapter.h"
#include "dsound_effects_adapter.h"
#include "fiber_adapter.h"
#include "input_adapter.h"
#include "kernel_abi.h"
#include "runtime.h"
#include "save_transaction.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

static size_t ram_size(void)
{
    return recomp_runtime.memory_region_count != 0u ? recomp_runtime.memory_regions[0].size : 0u;
}

enum {
    RAM_PAGE_BYTES = 0x1000u,
    RAM_PAGE_MAX = 0x4000u,
    RAM_IMAGE_MAX = 8u,
};

/* Guest RAM is write-watched, so a save, a rewind and a comparison touch
   only the pages written since the image they work from. Each page records
   the last collection it was dirty in, and each saved image the collection
   it was copied at. */
static uint32_t ram_page_generation[RAM_PAGE_MAX];
static uint32_t ram_generation = 1u;
static void *ram_dirty[RAM_PAGE_MAX];
static size_t ram_dirty_count;
static struct {
    const void *image;
    uint32_t generation;
} ram_images[RAM_IMAGE_MAX];

static void ram_collect_dirty(void)
{
    const uintptr_t base = (uintptr_t)recomp_runtime.memory_regions[0].data;
    const size_t size = ram_size();
    const size_t pages = size / RAM_PAGE_BYTES;
    bool tracked = false;

    ++ram_generation;
    ram_dirty_count = 0u;
#ifdef _WIN32
    {
        ULONG_PTR count = pages;
        DWORD granularity = 0u;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)base, size,
                ram_dirty, &count, &granularity) == 0u) {
            ram_dirty_count = (size_t)count;
            tracked = true;
        }
    }
#endif
    if (!tracked) {
        /* A host without write watch: assume every page moved. */
        for (size_t page = 0u; page < pages; ++page) ram_dirty[page] = (void *)(base + page * RAM_PAGE_BYTES);
        ram_dirty_count = pages;
    }
    for (size_t i = 0u; i < ram_dirty_count; ++i) {
        const size_t page = ((uintptr_t)ram_dirty[i] - base) / RAM_PAGE_BYTES;
        if (page < pages) ram_page_generation[page] = ram_generation;
    }
}

static int ram_image_find(const void *image, bool create)
{
    for (size_t i = 0u; i < RAM_IMAGE_MAX; ++i)
        if (ram_images[i].image == image) return (int)i;
    if (!create) return -1;
    for (size_t i = 0u; i < RAM_IMAGE_MAX; ++i) {
        if (ram_images[i].image != NULL) continue;
        ram_images[i].image = image;
        ram_images[i].generation = 0u;
        return (int)i;
    }
    return -1;
}

static void ram_save(void *state)
{
    const uint8_t *base = recomp_runtime.memory_regions[0].data;
    const size_t size = ram_size();
    uint8_t *image = state;
    const int slot = ram_image_find(state, true);
    uint32_t generation = 0u;

    ram_collect_dirty();
    if (slot >= 0) generation = ram_images[slot].generation;
    for (size_t page = 0u; page * RAM_PAGE_BYTES < size; ++page) {
        const size_t offset = page * RAM_PAGE_BYTES;
        if (ram_page_generation[page] <= generation) continue;
        memcpy(image + offset, base + offset, RAM_PAGE_BYTES);
    }
    if (slot >= 0) ram_images[slot].generation = ram_generation;
}

static void ram_restore(const void *state)
{
    uint8_t *base = recomp_runtime.memory_regions[0].data;
    const size_t size = ram_size();
    const uint8_t *image = state;
    const int slot = ram_image_find(state, false);
    uint32_t generation = 0u;

    ram_collect_dirty();
    if (slot >= 0) generation = ram_images[slot].generation;
    for (size_t page = 0u; page * RAM_PAGE_BYTES < size; ++page) {
        const size_t offset = page * RAM_PAGE_BYTES;
        if (ram_page_generation[page] <= generation) continue;
        memcpy(base + offset, image + offset, RAM_PAGE_BYTES);
    }
}

/* Audio worker counters and their copies on worker stacks advance with host
   scheduling, not with the frame. Rollback excludes audio the same way. */
static const struct { uint32_t offset, size; } ram_ignore[] = {
    {0xf2ad60u, 0x50u}, {0xf3e800u, 0x10u}, {0xf5cde0u, 0x20u}, {0x1056c80u, 0xa0u},
    /* QPC bookkeeping: frame start/elapsed/fps and the task scheduler's last
       QPC. These are host-time observations, not simulation state. */
    {0xdf3790u, 0x30u}, {0xa94810u, 0x60u},
};

typedef struct IgnoreRange { size_t start, stop; } IgnoreRange;

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
    for (size_t i = 0u; i < thread_stack_count && count < capacity; ++i)
        ranges[count++] = (IgnoreRange){thread_stacks[i].base,
            (size_t)thread_stacks[i].base + thread_stacks[i].size};
    RecompGuestStackRange kernel_stacks[16];
    const size_t kernel_stack_count = recomp_kernel_memory_stack_ranges(kernel_stacks, 16u);
    for (size_t i = 0u; i < kernel_stack_count && count < capacity; ++i)
        ranges[count++] = (IgnoreRange){kernel_stacks[i].base,
            (size_t)kernel_stacks[i].base + kernel_stacks[i].size};
    if (getenv("RECOMP_SYNCTEST_VERBOSE") != NULL) {
        for (size_t i = 0u; i < stack_count; ++i)
            fprintf(stderr, "recomp synctest: fiber stack +0x%x..+0x%x\n",
                stacks[i].base, stacks[i].base + stacks[i].size);
        for (size_t i = 0u; i < thread_stack_count; ++i)
            fprintf(stderr, "recomp synctest: thread stack +0x%x..+0x%x\n",
                thread_stacks[i].base, thread_stacks[i].base + thread_stacks[i].size);
    }
    /* Kernel data and the PCR page: host-scheduled kernel bookkeeping. */
    if (count < capacity) ranges[count++] = (IgnoreRange){0x03f00000u, 0x04000000u};
    if (count == capacity) {
        fprintf(stderr, "recomp synctest: ignore range list full\n");
        exit(2);
    }
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

static bool ram_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = left, *b = right;
    IgnoreRange ranges[64];
    const size_t range_count = build_ignore_ranges(ranges, 64u);
    const int left_slot = ram_image_find(left, false);
    const int right_slot = ram_image_find(right, false);
    uint32_t generation = 0u;
    size_t range = 0u;

    /* Only pages written since the older image was taken can differ. */
    if (left_slot >= 0 && right_slot >= 0) {
        const uint32_t left_generation = ram_images[left_slot].generation;
        const uint32_t right_generation = ram_images[right_slot].generation;
        generation = left_generation < right_generation ? left_generation : right_generation;
    }
    for (size_t i = 0u; i < size; ) {
        const size_t page = i / RAM_PAGE_BYTES;
        if (ram_page_generation[page] < generation) {
            i = (page + 1u) * RAM_PAGE_BYTES;
            continue;
        }
        while (range < range_count && ranges[range].stop <= i) ++range;
        if (range < range_count && i >= ranges[range].start) {
            i = ranges[range].stop;
            continue;
        }
        size_t end = i + 1u;
        if (range < range_count && ranges[range].start > i) end = ranges[range].start;
        if (memcmp(a + i, b + i, end - i) != 0) {
            if (getenv("RECOMP_SYNCTEST_VERBOSE") != NULL) {
                size_t at = i;
                while (a[at] == b[at]) ++at;
                fprintf(stderr, "recomp synctest: ram_equal rejects +0x%zx\n", at);
            }
            return false;
        }
        i = end;
    }
    return true;
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

void recomp_synctest_install_default_modules(void)
{
    RecompSyncTestModule table[15];
    size_t count = 0u;
    if (ram_size() != 0u)
        table[count++] = (RecompSyncTestModule){"guest_ram", ram_size(), ram_save, ram_restore,
            ram_equal, NULL, false};
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
        recomp_kernel_thread_state_save, recomp_kernel_thread_state_restore, NULL, NULL, true};
    table[count++] = (RecompSyncTestModule){"thread_fibers",
        recomp_kernel_thread_fiber_state_size(),
        recomp_kernel_thread_fiber_state_save, recomp_kernel_thread_fiber_state_restore,
        NULL, thread_fibers_restorable, true};
    table[count++] = (RecompSyncTestModule){"file_positions",
        recomp_kernel_file_state_size(),
        recomp_kernel_file_state_save, recomp_kernel_file_state_restore};
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
        recomp_dsound_effects_state_save, recomp_dsound_effects_state_restore};
    table[count++] = (RecompSyncTestModule){"save_storage", sizeof(SaveStorageState),
        save_storage_save, save_storage_restore, NULL, save_storage_restorable, true};
    recomp_synctest_set_modules(table, count);
}
