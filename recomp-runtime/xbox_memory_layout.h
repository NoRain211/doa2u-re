#ifndef DOAXBV_XBOX_MEMORY_LAYOUT_H
#define DOAXBV_XBOX_MEMORY_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* The final MiB is reserved for bootstrap state, outside both allocators. */
#define XBOX_GUEST_ALLOC_LIMIT 0x03f00000u
#define XBOX_STARTUP_STACK_BASE XBOX_GUEST_ALLOC_LIMIT
#define XBOX_KERNEL_DATA_BASE 0x03ff0000u
#define XBOX_KERNEL_DATA_SIZE 0x1000u
#define XBOX_STARTUP_STACK_TOP (XBOX_KERNEL_DATA_BASE - 0x10u)
#define XBOX_STARTUP_THREAD_OBJECT (XBOX_KERNEL_DATA_BASE + 0x500u)
#define XBOX_STARTUP_THREAD_STACK_SLOT (XBOX_KERNEL_DATA_BASE + XBOX_KERNEL_DATA_SIZE)
/* Physical page zero is guest-owned; keep FS state in the reserved kernel page. */
#define XBOX_PCR_BASE (XBOX_KERNEL_DATA_BASE + 0x800u)
#define XBOX_PCR_SIZE 0x400u

#ifdef __cplusplus
extern "C" {
#endif

/* Call once before allocating, after validating the image range in 64-bit arithmetic. */
bool xbox_HeapSetImageEnd(uint32_t image_end);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void xbox_HeapFree(uint32_t guest_address);
uint32_t xbox_HeapCheckpoint(void);
bool xbox_HeapRestore(uint32_t checkpoint);
typedef struct XboxHeapSnapshot {
    uint32_t image_bytes;
    uint32_t available_pages;
} XboxHeapSnapshot;
XboxHeapSnapshot xbox_HeapSnapshot(void);
uint32_t xbox_ContiguousAlloc(
    uint32_t size,
    uint32_t lowest_address,
    uint32_t highest_address,
    uint32_t alignment);

#ifdef __cplusplus
}
#endif

#endif
