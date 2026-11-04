#include "native_fiber.h"
#include "runtime.h"
#include "stop_report.h"

#ifdef _WIN32

static void *native_current(void)
{
    return IsThreadAFiber() ? GetCurrentFiber()
        : ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
}

void *recomp_native_fiber_create(LPFIBER_START_ROUTINE entry, void *parameter)
{
    return CreateFiberEx(0u, 0x100000u, FIBER_FLAG_FLOAT_SWITCH, entry, parameter);
}

static void native_switch(void *target)
{
    SwitchToFiber(target);
}

void recomp_native_fiber_delete(void *fiber)
{
    DeleteFiber(fiber);
}

void recomp_native_fiber_release_thread(void)
{
    (void)ConvertFiberToThread();
}

#else

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* POSIX hosts switch stacks with recomp_fiber_switch (fiber_switch_*.S). It
   saves the callee-saved registers on the outgoing stack and resumes the
   target's saved stack pointer. A new fiber's frame returns into
   recomp_fiber_trampoline, which calls entry(parameter). */
void recomp_fiber_switch(void **from_sp, void *to_sp);
void recomp_fiber_trampoline(void);

typedef struct RecompNativeFiber {
    void *sp;
    void *stack_memory;
    size_t stack_size;
} RecompNativeFiber;

static _Thread_local RecompNativeFiber *active_fiber;

static void *native_current(void)
{
    if (active_fiber == NULL) {
        active_fiber = calloc(1u, sizeof(*active_fiber));
    }
    return active_fiber;
}

void *recomp_native_fiber_create(LPFIBER_START_ROUTINE entry, void *parameter)
{
    /* The lowest page stays inaccessible, so an overflow faults here as it
       does against a Windows fiber's guard page. */
    const size_t guard_size = (size_t)sysconf(_SC_PAGESIZE);
    const size_t stack_size = 0x100000u + guard_size;
    void *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    RecompNativeFiber *fiber;
    uintptr_t top;
    uint64_t *frame;

    if (stack == MAP_FAILED) return NULL;
    if (mprotect(stack, guard_size, PROT_NONE) != 0) {
        munmap(stack, stack_size);
        return NULL;
    }
    fiber = calloc(1u, sizeof(*fiber));
    if (fiber == NULL) {
        munmap(stack, stack_size);
        return NULL;
    }
    fiber->stack_memory = stack;
    fiber->stack_size = stack_size;
    top = ((uintptr_t)stack + stack_size) & ~(uintptr_t)15u;
#if defined(__arm64__) || defined(__aarch64__)
    /* d8-d15 and x19-x30 in the order recomp_fiber_switch pops them. */
    top -= 160u;
    frame = (uint64_t *)top;
    for (size_t i = 0u; i < 20u; ++i) frame[i] = 0u;
    frame[9] = (uint64_t)(uintptr_t)recomp_fiber_trampoline; /* x30 */
    frame[18] = (uint64_t)(uintptr_t)parameter; /* x19 */
    frame[19] = (uint64_t)(uintptr_t)entry; /* x20 */
#elif defined(__x86_64__)
    /* r15, r14, r13, r12, rbx, rbp, return address, alignment pad. */
    top -= 64u;
    frame = (uint64_t *)top;
    for (size_t i = 0u; i < 8u; ++i) frame[i] = 0u;
    frame[2] = (uint64_t)(uintptr_t)entry; /* r13 */
    frame[3] = (uint64_t)(uintptr_t)parameter; /* r12 */
    frame[6] = (uint64_t)(uintptr_t)recomp_fiber_trampoline;
#else
#error "Unsupported architecture for native POSIX fiber switching"
#endif
    fiber->sp = (void *)top;
    return fiber;
}

static void native_switch(void *target)
{
    RecompNativeFiber *from = native_current();

    active_fiber = target;
    recomp_fiber_switch(&from->sp, active_fiber->sp);
}

void recomp_native_fiber_delete(void *handle)
{
    RecompNativeFiber *fiber = handle;

    if (fiber == NULL) return;
    if (fiber->stack_memory != NULL) {
        munmap(fiber->stack_memory, fiber->stack_size);
    }
    if (active_fiber == fiber) active_fiber = NULL;
    free(fiber);
}

void recomp_native_fiber_release_thread(void)
{
    free(active_fiber);
    active_fiber = NULL;
}

#endif

void *recomp_native_fiber_current(void)
{
    void *fiber = native_current();
    if (fiber == NULL) recomp_stop(2, "fiber:convert-failed");
    return fiber;
}

void recomp_native_fiber_switch(void *target)
{
    /* Each native continuation owns its diagnostic call stack, including
       switches between game fibers within a scheduled guest thread. */
    RecompDispatchContext saved;
    recomp_dispatch_context_save(&saved);
    recomp_dispatch_context_clear();
    native_switch(target);
    recomp_dispatch_context_restore(&saved);
}
