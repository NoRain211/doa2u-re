#include "xbox_memory_layout.h"

enum {
    XBOX_CACHED_ALIAS = 0x80000000u,
    XBOX_PAGE_SIZE = 0x1000u,
};

static uint32_t heap_base;
static uint32_t heap_cursor;
static uint32_t contiguous_cursor = XBOX_GUEST_ALLOC_LIMIT;

bool xbox_HeapSetImageEnd(uint32_t image_end)
{
    uint64_t aligned = ((uint64_t)image_end + XBOX_PAGE_SIZE - 1u) &
        ~(uint64_t)(XBOX_PAGE_SIZE - 1u);
    if (image_end == 0u || aligned >= XBOX_GUEST_ALLOC_LIMIT || heap_base != 0u) {
        return false;
    }
    heap_base = (uint32_t)aligned;
    heap_cursor = heap_base;
    return true;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    uint64_t aligned;
    uint64_t end;

    if (heap_base == 0u || alignment < 4u ||
        (alignment & (alignment - 1u)) != 0u) {
        return 0u;
    }
    aligned = ((uint64_t)heap_cursor + alignment - 1u) & ~(uint64_t)(alignment - 1u);
    end = aligned + size;
    if (end > contiguous_cursor) {
        return 0u;
    }
    heap_cursor = (uint32_t)end;
    return (uint32_t)aligned;
}

void xbox_HeapFree(uint32_t guest_address)
{
    (void)guest_address;
}

uint32_t xbox_HeapCheckpoint(void)
{
    return heap_cursor;
}

bool xbox_HeapRestore(uint32_t checkpoint)
{
    if (heap_base == 0u || checkpoint < heap_base || checkpoint > heap_cursor) {
        return false;
    }
    heap_cursor = checkpoint;
    return true;
}

XboxHeapSnapshot xbox_HeapSnapshot(void)
{
    uint32_t first_page = (heap_cursor + XBOX_PAGE_SIZE - 1u) / XBOX_PAGE_SIZE;
    uint32_t last_page = contiguous_cursor / XBOX_PAGE_SIZE;
    return (XboxHeapSnapshot){heap_base,
        heap_base != 0u && last_page > first_page ? last_page - first_page : 0u};
}

uint32_t xbox_ContiguousAlloc(
    uint32_t size,
    uint32_t lowest_address,
    uint32_t highest_address,
    uint32_t alignment)
{
    uint64_t rounded_size;
    uint64_t upper_bound;
    uint64_t base;

    if (heap_base == 0u || size == 0u) {
        return 0u;
    }
    if (alignment < XBOX_PAGE_SIZE) {
        alignment = XBOX_PAGE_SIZE;
    }
    if ((alignment & (alignment - 1u)) != 0u) {
        return 0u;
    }
    rounded_size = ((uint64_t)size + XBOX_PAGE_SIZE - 1u) &
        ~(uint64_t)(XBOX_PAGE_SIZE - 1u);
    upper_bound = (uint64_t)highest_address + 1u;
    if (upper_bound > contiguous_cursor) {
        upper_bound = contiguous_cursor;
    }
    if (rounded_size > upper_bound) {
        return 0u;
    }
    base = (upper_bound - rounded_size) & ~(uint64_t)(alignment - 1u);
    if (base < lowest_address || base < heap_cursor) {
        return 0u;
    }
    contiguous_cursor = (uint32_t)base;
    return XBOX_CACHED_ALIAS | (uint32_t)base;
}
