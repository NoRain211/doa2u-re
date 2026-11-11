#include "xbox_memory_layout.h"

#include <stdio.h>

int main(void)
{
    const uint32_t image_end = 0x01012345u;
    const uint32_t image_floor = 0x01013000u;
    const uint64_t pcr_end = (uint64_t)XBOX_PCR_BASE + XBOX_PCR_SIZE;
    uint32_t heap;
    uint32_t resource;
    uint32_t physical;
    uint32_t checkpoint;

    if (XBOX_RUNNER_ASSET_BASE != XBOX_GUEST_ALLOC_LIMIT ||
        XBOX_RUNNER_ASSET_BASE + XBOX_RUNNER_ASSET_SIZE != XBOX_STARTUP_STACK_BASE ||
        XBOX_STARTUP_STACK_TOP < XBOX_STARTUP_STACK_BASE ||
        XBOX_STARTUP_STACK_TOP >= XBOX_KERNEL_DATA_BASE ||
        XBOX_STARTUP_THREAD_OBJECT + 0x200u >
            XBOX_KERNEL_DATA_BASE + XBOX_KERNEL_DATA_SIZE ||
        XBOX_STARTUP_THREAD_STACK_SLOT + 4u + 0x1000u > 0x04000000u) {
        fprintf(stderr, "guest heap: bootstrap reservations overlap\n");
        return 1;
    }
    if (XBOX_PCR_BASE < XBOX_KERNEL_DATA_BASE + 0x290u ||
        XBOX_PCR_BASE < XBOX_STARTUP_THREAD_OBJECT + 0x200u ||
        XBOX_PCR_BASE < XBOX_STARTUP_STACK_TOP + 0x10u ||
        pcr_end > XBOX_KERNEL_DATA_BASE + XBOX_KERNEL_DATA_SIZE ||
        pcr_end > XBOX_STARTUP_THREAD_STACK_SLOT || XBOX_PCR_SIZE == 0u) {
        fprintf(stderr, "guest heap: PCR overlaps another bootstrap reservation\n");
        return 1;
    }
    if (xbox_HeapAlloc(4u, 4u) != 0u ||
        xbox_ContiguousAlloc(0x1000u, 0u, UINT32_MAX, 0x1000u) != 0u ||
        xbox_HeapCheckpoint() != 0u || xbox_HeapRestore(0u) ||
        xbox_HeapSnapshot().image_bytes != 0u || xbox_HeapSnapshot().available_pages != 0u) {
        fprintf(stderr, "guest heap: allocation before image initialization\n");
        return 1;
    }
    if (xbox_HeapSetImageEnd(0u) || xbox_HeapSetImageEnd(UINT32_MAX) ||
        xbox_HeapSetImageEnd(XBOX_GUEST_ALLOC_LIMIT) ||
        xbox_HeapSetImageEnd(XBOX_GUEST_ALLOC_LIMIT - 1u) ||
        !xbox_HeapSetImageEnd(image_end) || xbox_HeapSetImageEnd(image_end) ||
        xbox_HeapCheckpoint() != image_floor ||
        xbox_HeapSnapshot().image_bytes != image_floor ||
        xbox_HeapSnapshot().available_pages != (XBOX_GUEST_ALLOC_LIMIT - image_floor) / 0x1000u) {
        fprintf(stderr, "guest heap: invalid image boundary accepted\n");
        return 1;
    }
    if (xbox_HeapAlloc(4u, 3u) != 0u ||
        xbox_HeapAlloc(4u, 12u) != 0u ||
        xbox_HeapAlloc(UINT32_MAX, 4u) != 0u ||
        xbox_ContiguousAlloc(0u, 0u, UINT32_MAX, 0x1000u) != 0u ||
        xbox_ContiguousAlloc(1u, 0u, UINT32_MAX, 0x1800u) != 0u ||
        xbox_ContiguousAlloc(UINT32_MAX, 0u, UINT32_MAX, 0x1000u) != 0u ||
        xbox_ContiguousAlloc(1u, 0u, image_floor - 1u, 0x1000u) != 0u ||
        xbox_ContiguousAlloc(1u, XBOX_GUEST_ALLOC_LIMIT, UINT32_MAX, 0x1000u) != 0u ||
        xbox_HeapCheckpoint() != image_floor) {
        fprintf(stderr, "guest heap: invalid size, alignment or range accepted\n");
        return 1;
    }

    heap = xbox_HeapAlloc(0x500000u, 0x1000u);
    resource = xbox_ContiguousAlloc(0x2000000u, 0u, UINT32_MAX, 0x1000u);
    physical = resource & 0x7fffffffu;
    if (heap != image_floor || heap < image_end || resource == 0u ||
        physical < heap + 0x500000u ||
        physical + 0x2000000u != XBOX_GUEST_ALLOC_LIMIT) {
        fprintf(stderr, "guest heap: image, heap or bootstrap reservation overlapped\n");
        return 1;
    }
    checkpoint = xbox_HeapCheckpoint();
    if (xbox_HeapSnapshot().available_pages != (physical - checkpoint) / 0x1000u) {
        fprintf(stderr, "guest heap: snapshot ignored live reservations\n");
        return 1;
    }
    if (xbox_HeapRestore(image_floor - 1u) || xbox_HeapRestore(checkpoint + 1u) ||
        xbox_HeapAlloc(physical - checkpoint, 4u) != checkpoint ||
        xbox_HeapAlloc(4u, 4u) != 0u ||
        xbox_ContiguousAlloc(0x1000u, 0u, UINT32_MAX, 0x1000u) != 0u ||
        xbox_HeapCheckpoint() != physical || xbox_HeapSnapshot().available_pages != 0u ||
        !xbox_HeapRestore(checkpoint) ||
        xbox_HeapSnapshot().available_pages != (physical - checkpoint) / 0x1000u) {
        fprintf(stderr, "guest heap: live allocator boundaries crossed\n");
        return 1;
    }
    if (xbox_ContiguousAlloc(1u, physical - 0x1000u,
            physical - 1u, 4u) != (0x80000000u | (physical - 0x1000u)) ||
        xbox_ContiguousAlloc(1u, physical - 0x1000u,
            physical - 1u, 4u) != 0u ||
        xbox_ContiguousAlloc(1u, 0u, image_end - 1u, 0x1000u) != 0u ||
        xbox_HeapCheckpoint() != checkpoint) {
        fprintf(stderr, "guest heap: contiguous page rounding or inclusive bounds failed\n");
        return 1;
    }
    return 0;
}
