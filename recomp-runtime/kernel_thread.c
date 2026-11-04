#include "kernel_abi.h"
#include "stop_report.h"
#include "xapi_time_adapter.h"
#include "xbox_memory_layout.h"
#include "native_fiber.h"
#include "fiber_adapter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    DISPATCHER_SIGNAL_STATE = 0x04u,
    TIMER_DUE_TIME = 0x10u,
    TIMER_DPC = 0x20u,
    MAX_EVENTS = 64u,
    MAX_THREADS = 32u,
};

static const uint32_t THREAD_STATUS_SUCCESS = 0x00000000u;
static const uint32_t THREAD_STATUS_USER_APC = 0x000000c0u;
static const uint32_t THREAD_STATUS_INVALID_HANDLE = 0xc0000008u;
static const uint32_t THREAD_STATUS_INVALID_PARAMETER = 0xc000000du;
static const uint32_t THREAD_STATUS_TIMEOUT = 0x00000102u;

typedef struct SyntheticEvent {
    uint32_t handle;
    uint32_t object;
} SyntheticEvent;

static SyntheticEvent events[MAX_EVENTS];
static uint32_t next_event_handle = 0xbee20000u;

typedef struct UserApc {
    uint32_t routine;
    uint32_t context;
    uint32_t io_status_block;
} UserApc;

typedef struct GuestThread {
    uint32_t handle;
    bool handle_closed;
    uint32_t object;
    void *native_fiber;
    /* XAPI fiber current in this thread when it is switched out. */
    uint32_t guest_fiber;
    RecompRegisters registers;
    RecompFpuContext fpu;
    uint32_t fs[0x60u / 4u];
    uint32_t entry;
    uint32_t suspend_count;
    int32_t priority;
    int32_t base_increment;
    bool exited;
    bool waiting;
    bool alertable;
    bool wait_all;
    uint32_t objects[MAX_EVENTS];
    uint32_t object_count;
    uint64_t deadline;
    uint32_t wait_result;
    UserApc apcs[MAX_EVENTS];
    unsigned apc_count;
    /* Host work this wait ends on (recomp_kernel_wait_host), or NULL. */
    bool (*host_done)(void *context);
    void *host_context;
} GuestThread;

/* shortcut: retain thread records until exit; add reclamation if a title
   creates more than 31 worker threads. */
static GuestThread threads[MAX_THREADS] = {
    {.handle = 1u, .object = XBOX_STARTUP_THREAD_OBJECT, .priority = 8},
};
static unsigned current_thread;
static bool scheduler_started;
static unsigned host_waits; /* Threads blocked in recomp_kernel_wait_host. */

/* RECOMP_PERF_COUNTER frame breakdown: host QPC ticks each guest thread ran,
   and ticks the scheduler slept with nothing runnable, since the last report. */
static int64_t run_ticks[MAX_THREADS], idle_ticks, slice_start;

static void charge_slice(bool idle)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (slice_start != 0) {
        if (idle) idle_ticks += now.QuadPart - slice_start;
        else run_ticks[current_thread] += now.QuadPart - slice_start;
    }
    slice_start = now.QuadPart;
}

/* Appends " idle=<ms> <handle>=<ms>..." for threads that ran at least 0.5 ms, then resets. */
void recomp_kernel_thread_times(char *out, size_t size)
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    const double ms = 1000.0 / (double)frequency.QuadPart;
    charge_slice(false);
    size_t used = (size_t)snprintf(out, size, " idle=%.1f", idle_ticks * ms);
    idle_ticks = 0;
    for (unsigned i = 0u; i < MAX_THREADS; ++i) {
        if (run_ticks[i] * ms >= 0.5 && used < size)
            used += (size_t)snprintf(out + used, size - used, " %08x=%.1f", threads[i].handle, run_ticks[i] * ms);
        run_ticks[i] = 0;
    }
}

uint32_t recomp_kernel_current_thread_id(void)
{
    return threads[current_thread].handle;
}

static GuestThread *find_thread(uint32_t handle)
{
    if (handle == 0xfffffffeu) return &threads[current_thread];
    for (unsigned i = 0u; i < MAX_THREADS; ++i) {
        if (threads[i].handle != 0u &&
            ((!threads[i].handle_closed && threads[i].handle == handle) ||
             threads[i].object == handle)) {
            return &threads[i];
        }
    }
    return NULL;
}

uint32_t recomp_kernel_close_thread_handle(uint32_t handle)
{
    for (unsigned i = 0u; i < MAX_THREADS; ++i) {
        GuestThread *thread = &threads[i];
        if (thread->handle != 0u && thread->handle == handle && !thread->handle_closed) {
            // Closing the public handle keeps the runnable thread and join object alive.
            thread->handle_closed = true;
            return THREAD_STATUS_SUCCESS;
        }
    }
    return THREAD_STATUS_INVALID_HANDLE;
}

void recomp_kernel_queue_user_apc(
    uint32_t routine, uint32_t context, uint32_t io_status_block)
{
    GuestThread *thread = &threads[current_thread];
    if (thread->apc_count == MAX_EVENTS) {
        recomp_stop(2, "kernel:user-apc-queue-full");
    }
    thread->apcs[thread->apc_count++] =
        (UserApc){routine, context, io_status_block};
}

static int deliver_user_apcs(void)
{
    GuestThread *thread = &threads[current_thread];
    RecompRegisters saved = recomp_runtime.registers;
    unsigned delivered = 0u;
    while (thread->apc_count != 0u) {
        UserApc apc = thread->apcs[0];
        const uint32_t arguments[3] = {apc.context, apc.io_status_block, 0u};
        --thread->apc_count;
        memmove(thread->apcs, thread->apcs + 1u,
                thread->apc_count * sizeof(UserApc));
        ++delivered;
        kernel_call_guest(apc.routine, arguments, 3u);
    }
    recomp_runtime.registers = saved;
    return delivered != 0u;
}

static uint64_t interrupt_time(void)
{
    uint64_t count = recomp_xapi_performance_counter();
    uint64_t freq = recomp_xapi_performance_frequency();
    return count / freq * 10000000ull + count % freq * 10000000ull / freq;
}

static uint64_t system_time(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        recomp_stop(2, "thread:system-time");
    }
    return ((uint64_t)now.tv_sec + 11644473600ull) * 10000000ull +
        (uint64_t)now.tv_nsec / 100ull;
}

static uint64_t wait_deadline(uint32_t timeout)
{
    if (timeout == 0u) return UINT64_MAX;
    int64_t interval = (int64_t)*recomp_memory_u64(timeout);
    uint64_t now = interrupt_time();
    uint64_t delta;
    if (interval <= 0) {
        delta = 0u - (uint64_t)interval;
    } else {
        uint64_t utc = system_time();
        delta = (uint64_t)interval > utc ? (uint64_t)interval - utc : 0u;
    }
    return delta >= UINT64_MAX - now ? UINT64_MAX - 1u : now + delta;
}

/* Timers armed with a DPC. Their DPC is queued at expiry, which the DPC drain
   and the idle scheduler poll; nothing raises a clock interrupt. */
static uint32_t armed_timers[MAX_EVENTS];

static bool expire_timer(uint32_t timer)
{
    uint64_t due = *recomp_memory_u64(timer + TIMER_DUE_TIME);
    if (due == 0u || interrupt_time() < due) return false;
    *recomp_memory_u32(timer + DISPATCHER_SIGNAL_STATE) = 1u;
    *recomp_memory_u64(timer + TIMER_DUE_TIME) = 0u;
    uint32_t dpc = *recomp_memory_u32(timer + TIMER_DPC);
    return dpc != 0u && recomp_kernel_queue_dpc(dpc, 0u, 0u) != 0u;
}

unsigned recomp_kernel_expire_timers(void)
{
    unsigned queued = 0u;
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (armed_timers[i] == 0u) continue;
        queued += expire_timer(armed_timers[i]);
        if (*recomp_memory_u64(armed_timers[i] + TIMER_DUE_TIME) == 0u) armed_timers[i] = 0u;
    }
    return queued;
}

static void arm_timer(uint32_t timer)
{
    unsigned free_slot = MAX_EVENTS;
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (armed_timers[i] == timer) return;
        if (armed_timers[i] == 0u && free_slot == MAX_EVENTS) free_slot = i;
    }
    if (free_slot == MAX_EVENTS) recomp_stop(2, "thread:timer-capacity");
    armed_timers[free_slot] = timer;
}

/* A cancelled or reinitialized timer's storage may be reused by the guest. */
static void disarm_timer(uint32_t timer)
{
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (armed_timers[i] == timer) armed_timers[i] = 0u;
    }
}

static SyntheticEvent *find_event(uint32_t handle)
{
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (events[i].handle == handle) {
            return &events[i];
        }
    }
    return NULL;
}

static SyntheticEvent *create_event(uint32_t type, uint32_t initial_state)
{
    for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
        if (events[i].handle == 0u) {
            events[i].handle = next_event_handle;
            events[i].object = xbox_HeapAlloc(0x10u, 4u);
            if (events[i].object == 0u) {
                events[i].handle = 0u;
                return NULL;
            }
            recomp_guest_memset(events[i].object, 0, 0x10u);
            *recomp_memory_u32(events[i].object) = type;
            *recomp_memory_u32(events[i].object + 4u) = initial_state != 0u;
            next_event_handle += 4u;
            return &events[i];
        }
    }
    return NULL;
}

static uint32_t wait_object(uint32_t value, bool handle)
{
    GuestThread *thread = find_thread(value);
    SyntheticEvent *event = find_event(value);
    if (thread != NULL) return thread->object;
    if (event != NULL) return event->object;
    return handle ? 0u : value;
}

static bool object_signaled(uint32_t object)
{
    GuestThread *thread = find_thread(object);
    if (thread != NULL) return thread->exited;
    uint32_t type = (uint8_t)*recomp_memory_i8(object);
    if (type == 8u || type == 9u) expire_timer(object);
    return (int32_t)*recomp_memory_u32(object + DISPATCHER_SIGNAL_STATE) > 0;
}

static void consume_object(uint32_t object)
{
    uint32_t type;
    if (find_thread(object) != NULL) return;
    type = (uint8_t)*recomp_memory_i8(object);
    if (type == 1u || type == 9u) {
        *recomp_memory_u32(object + DISPATCHER_SIGNAL_STATE) = 0u;
    } else if (type == 5u) {
        --*recomp_memory_u32(object + DISPATCHER_SIGNAL_STATE);
    } else if (type != 0u && type != 8u) {
        recomp_stop(2, "thread:unsupported-wait-object:%u", type);
    }
}

static bool thread_ready(GuestThread *thread)
{
    if (thread->handle == 0u || thread->exited || thread->suspend_count != 0u)
        return false;
    if (!thread->waiting) return true;
    if (thread->host_done != NULL) {
        if (!thread->host_done(thread->host_context)) return false;
        thread->host_done = NULL;
        --host_waits;
        thread->wait_result = THREAD_STATUS_SUCCESS;
    } else if (thread->alertable && thread->apc_count != 0u) {
        thread->wait_result = THREAD_STATUS_USER_APC;
    } else {
        uint32_t signaled = 0u;
        for (uint32_t i = 0u; i < thread->object_count; ++i) {
            if (object_signaled(thread->objects[i])) {
                if (!thread->wait_all) {
                    consume_object(thread->objects[i]);
                    thread->wait_result = i;
                    thread->waiting = false;
                    return true;
                }
                ++signaled;
            }
        }
        if (thread->object_count != 0u && signaled == thread->object_count) {
            for (uint32_t i = 0u; i < thread->object_count; ++i)
                consume_object(thread->objects[i]);
            thread->wait_result = THREAD_STATUS_SUCCESS;
        } else if (interrupt_time() >= thread->deadline) {
            thread->wait_result = thread->object_count == 0u
                ? THREAD_STATUS_SUCCESS : THREAD_STATUS_TIMEOUT;
        } else {
            return false;
        }
    }
    thread->waiting = false;
    return true;
}

static void switch_thread(unsigned index)
{
    GuestThread *outgoing = &threads[current_thread];
    GuestThread *target = &threads[index];
    if (target == outgoing) return;
    charge_slice(false);
    outgoing->registers = recomp_runtime.registers;
    recomp_fpu_context_save(&outgoing->fpu);
    recomp_guest_load(outgoing->fs, XBOX_PCR_BASE, sizeof outgoing->fs);
    outgoing->guest_fiber = recomp_fiber_thread_context();
    outgoing->native_fiber = recomp_native_fiber_current();
    recomp_runtime.registers = target->registers;
    recomp_fpu_context_restore(&target->fpu);
    recomp_guest_store(XBOX_PCR_BASE, target->fs, sizeof target->fs);
    recomp_fiber_thread_restore(target->guest_fiber);
    current_thread = index;
    recomp_native_fiber_switch(target->native_fiber);
}

static void preempt_higher_priority(void)
{
    unsigned selected = current_thread;
    for (unsigned step = 1u; step < MAX_THREADS; ++step) {
        unsigned index = (current_thread + step) % MAX_THREADS;
        if (threads[index].priority > threads[selected].priority &&
            thread_ready(&threads[index])) {
            selected = index;
        }
    }
    if (selected != current_thread) switch_thread(selected);
}

static void schedule(void)
{
    GuestThread *outgoing = &threads[current_thread];
#ifdef _WIN32
    static HANDLE idle_timer;
#endif
    for (;;) {
        uint64_t earliest = UINT64_MAX;
        bool host_wait = false;
        /* Idle time is when the real kernel would run expired timer DPCs. */
        if (recomp_kernel_expire_timers() != 0u) {
            RecompRegisters saved = recomp_runtime.registers;
            recomp_kernel_drain_dpcs();
            recomp_runtime.registers = saved;
        }
        for (unsigned i = 0u; i < MAX_EVENTS; ++i) {
            uint64_t due = armed_timers[i] ? *recomp_memory_u64(armed_timers[i] + TIMER_DUE_TIME) : 0u;
            if (due != 0u && due < earliest) earliest = due;
        }
        for (unsigned step = 1u; step <= MAX_THREADS; ++step) {
            unsigned index = (current_thread + step) % MAX_THREADS;
            GuestThread *target = &threads[index];
            if (thread_ready(target)) {
                if (target == outgoing) return;
                switch_thread(index);
                return;
            }
            if (target->waiting && !target->exited && target->suspend_count == 0u) {
                if (target->host_done != NULL) host_wait = true;
                if (target->deadline < earliest) earliest = target->deadline;
                for (uint32_t i = 0u; i < target->object_count; ++i) {
                    uint32_t object = target->objects[i];
                    if (find_thread(object) != NULL) continue;
                    uint32_t type = (uint8_t)*recomp_memory_i8(object);
                    if (type == 8u || type == 9u) {
                        uint64_t due = *recomp_memory_u64(object + TIMER_DUE_TIME);
                        if (due != 0u && due < earliest) earliest = due;
                    }
                }
            }
        }
        /* Advance idle guest time in unpaced mode. Indefinite waits still
           sleep, and host work takes host time, so it is never skipped. */
        if (!host_wait && earliest < UINT64_MAX / 100u &&
            recomp_xapi_skip_wait(earliest * 100u)) continue;
        /* With no runnable thread, sleep until the first deadline, or poll
           host work each millisecond. */
        uint64_t now = interrupt_time();
        if (host_wait && earliest > now + 10000u) earliest = now + 10000u;
        charge_slice(false);
        uint64_t ms = earliest == UINT64_MAX ? 1000u
            : earliest > now ? (earliest - now + 9999u) / 10000u : 1u;
        if (earliest != UINT64_MAX && earliest > now) {
#ifdef _WIN32
            if (idle_timer == NULL) {
                idle_timer = CreateWaitableTimerExW(NULL, NULL,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_MODIFY_STATE | SYNCHRONIZE);
            }
            LARGE_INTEGER due;
            uint64_t remaining = earliest - now;
            due.QuadPart = -(int64_t)(remaining > 10000000u ? 10000000u : remaining);
            if (idle_timer != NULL &&
                SetWaitableTimer(idle_timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(idle_timer, INFINITE);
                charge_slice(true);
                continue;
            }
#else
            /* nanosleep has the resolution the Windows high-resolution
               timer gives; cap each wait at a second like the timer. */
            uint64_t remaining = earliest - now;
            if (remaining > 10000000u) remaining = 10000000u;
            struct timespec wait = {
                .tv_sec = (time_t)(remaining / 10000000u),
                .tv_nsec = (long)(remaining % 10000000u) * 100L,
            };
            nanosleep(&wait, NULL);
            charge_slice(true);
            continue;
#endif
        }
#ifdef _WIN32
        Sleep((DWORD)(ms > 1000u ? 1000u : ms));
#else
        if (ms > 1000u) ms = 1000u;
        struct timespec pause = {
            .tv_sec = (time_t)(ms / 1000u),
            .tv_nsec = (long)(ms % 1000u) * 1000000L,
        };
        nanosleep(&pause, NULL);
#endif
        charge_slice(true);
    }
}

void recomp_kernel_poll(void)
{
    static uint64_t next_slice;
    if (!scheduler_started) return;
    const uint64_t now = recomp_xapi_performance_counter();
    if (now < next_slice) return;
    next_slice = now + UINT64_C(1000000);
    unsigned selected = current_thread;
    for (unsigned step = 1u; step < MAX_THREADS; ++step) {
        unsigned index = (current_thread + step) % MAX_THREADS;
        if (threads[index].priority >= threads[current_thread].priority &&
            (selected == current_thread || threads[index].priority > threads[selected].priority) &&
            thread_ready(&threads[index])) {
            selected = index;
        }
    }
    if (selected != current_thread) switch_thread(selected);
}

/* Vblank waiters are blocked until the shared refresh deadline. Merely
   yielding leaves them runnable and priority changes can run them again
   before the refresh, serializing peer waits with presentation. */
void recomp_kernel_wait_for_vblank(uint64_t deadline_ns)
{
    if (!scheduler_started) return;
    GuestThread *thread = &threads[current_thread];
    thread->object_count = 0u;
    thread->alertable = false;
    thread->deadline = deadline_ns / 100u + (deadline_ns % 100u != 0u);
    thread->waiting = true;
    if (!thread_ready(thread)) schedule();
}

bool recomp_kernel_wait_host(bool (*done)(void *context), void *context)
{
    if (!scheduler_started) return false;
    GuestThread *thread = &threads[current_thread];
    thread->host_done = done;
    thread->host_context = context;
    thread->object_count = 0u;
    thread->alertable = false;
    thread->deadline = UINT64_MAX;
    thread->waiting = true;
    ++host_waits;
    if (!thread_ready(thread)) schedule();
    return true;
}

/* Host work ends without a guest interrupt, so a thread polling for its result
   never waits and would starve the thread that awaits it. Lock releases offer
   the same bounded slice as clock reads while host work is pending. */
void recomp_kernel_poll_host_waits(void)
{
    if (host_waits != 0u) recomp_kernel_poll();
}

static uint32_t wait_objects(uint32_t count, const uint32_t *objects,
    bool wait_all, bool alertable, uint32_t timeout)
{
    GuestThread *thread = &threads[current_thread];
    thread->object_count = count;
    if (count != 0u) memcpy(thread->objects, objects, count * sizeof(uint32_t));
    thread->wait_all = wait_all;
    thread->alertable = alertable;
    thread->deadline = wait_deadline(timeout);
    thread->waiting = true;
    if (!thread_ready(thread)) schedule();
    uint32_t result = thread->wait_result;
    if (result == THREAD_STATUS_USER_APC) (void)deliver_user_apcs();
    return result;
}

/* The XBE bootstrap may return after creating the real startup thread. */
void recomp_kernel_run_threads(void)
{
    for (unsigned i = 1u; i < MAX_THREADS; ++i) {
        if (threads[i].handle != 0u && !threads[i].exited) {
            uint32_t object = threads[i].object;
            (void)wait_objects(1u, &object, false, false, 0u);
        }
    }
}

static void exit_thread(uint32_t status)
{
    GuestThread *thread = &threads[current_thread];
    fprintf(stderr, "recomp thread: exit handle=0x%08x status=0x%08x\n",
            thread->handle, status);
    thread->exited = true;
    thread->apc_count = 0u;
    *recomp_memory_u32(thread->object + DISPATCHER_SIGNAL_STATE) = 1u;
    schedule();
    recomp_stop(2, "thread:resumed-after-exit");
}

static VOID WINAPI thread_entry(void *parameter)
{
    GuestThread *thread = parameter;
    fprintf(stderr, "recomp thread: start handle=0x%08x entry=0x%08x\n",
            thread->handle, thread->entry);
    recomp_dispatch_indirect_site(thread->entry,
        thread->registers.esp + 12u, __FILE__, __LINE__);
    exit_thread(recomp_runtime.registers.eax);
}

static void bridge_ps_create_system_thread_ex(void)
{
    uint32_t handle_pointer = kernel_arg(1u);
    uint32_t extension = kernel_arg(2u);
    uint32_t stack_size = kernel_arg(3u);
    uint32_t tls_size = kernel_arg(4u);
    uint32_t id_pointer = kernel_arg(5u);
    uint32_t context1 = kernel_arg(6u);
    uint32_t context2 = kernel_arg(7u);
    uint32_t suspended = kernel_arg(8u);
    uint32_t entry = kernel_arg(10u);
    GuestThread *thread = NULL;
    if (handle_pointer == 0u || context1 == 0u ||
        stack_size > 0x1000000u || tls_size > 0x100000u || extension > 0x10000u) {
        kernel_return(10u, THREAD_STATUS_INVALID_PARAMETER);
        return;
    }
    for (unsigned i = 1u; i < MAX_THREADS; ++i) {
        if (threads[i].handle == 0u) { thread = &threads[i]; break; }
    }
    if (thread == NULL) recomp_stop(2, "thread:capacity");
    if (stack_size < 0x4000u) stack_size = 0x4000u;
    stack_size = (stack_size + 0xfffu) & ~0xfffu;
    uint32_t allocation = xbox_HeapAlloc(stack_size + tls_size + extension + 0x204u, 0x1000u);
    if (allocation == 0u) {
        kernel_return(10u, 0xc000009au);
        return;
    }
    recomp_guest_memset(allocation, 0, stack_size + tls_size + extension + 0x204u);
    thread->handle = 0xbee10000u + (uint32_t)(thread - threads - 1u) * 4u;
    thread->object = allocation;
    uint32_t stack_base = allocation + 0x200u + extension;
    uint32_t tls_slot = stack_base + stack_size;
    thread->entry = entry != 0u ? entry : context1;
    thread->guest_fiber = 0u;
    thread->suspend_count = suspended != 0u;
    thread->priority = 8;
    thread->registers.esp = tls_slot - 12u;
    thread->fpu.fpu_control_word = 0x027fu;
    thread->fs[0] = 0xffffffffu;
    /* TlsDataSize includes the pointer slot used by XAPI's negative index. */
    thread->fs[1] = tls_slot + tls_size;
    thread->fs[2] = stack_base;
    thread->fs[0x1cu / 4u] = XBOX_PCR_BASE;
    thread->fs[0x20u / 4u] = XBOX_PCR_BASE + 0x28u;
    thread->fs[0x28u / 4u] = thread->object;
    *recomp_memory_u32(thread->object) = 6u;
    *recomp_memory_u32(thread->object + 0x1cu) = thread->fs[1];
    *recomp_memory_u32(thread->object + 0x20u) = stack_base;
    *recomp_memory_u32(thread->object + 0x28u) = tls_slot;
    *recomp_memory_u32(thread->object + 0x12cu) = thread->handle;
    *recomp_memory_u32(thread->registers.esp + 4u) = entry ? context1 : context2;
    *recomp_memory_u32(thread->registers.esp + 8u) = context2;
    thread->native_fiber = recomp_native_fiber_create(thread_entry, thread);
    if (thread->native_fiber == NULL) recomp_stop(2, "thread:create-fiber");
    if (!scheduler_started) {
        threads[0].native_fiber = recomp_native_fiber_current();
        *recomp_memory_u32(threads[0].object + 0x12cu) = threads[0].handle;
        scheduler_started = true;
    }
    *recomp_memory_u32(handle_pointer) = thread->handle;
    if (id_pointer != 0u) *recomp_memory_u32(id_pointer) = thread->handle;
    fprintf(stderr, "recomp thread: created handle=0x%08x start=0x%08x "
            "context1=0x%08x context2=0x%08x suspended=%u\n",
            thread->handle, entry, context1, context2, thread->suspend_count);
    kernel_return(10u, THREAD_STATUS_SUCCESS);
}

static void bridge_ps_terminate_system_thread(void)
{
    exit_thread(kernel_arg(1u));
}

static void bridge_ke_cancel_timer(void)
{
    uint32_t timer = kernel_arg(1u);

    if (timer != 0u) {
        uint32_t dpc = *recomp_memory_u32(timer + TIMER_DPC);

        if (dpc != 0u) {
            (void)recomp_kernel_remove_dpc(dpc);
        }
        *recomp_memory_u32(timer + TIMER_DPC) = 0u;
        *recomp_memory_u64(timer + TIMER_DUE_TIME) = 0u;
        disarm_timer(timer);
    }
    kernel_return(1u, 0u);
}

static void bridge_ke_delay_execution_thread(void)
{
    uint32_t alertable = kernel_arg(2u);
    uint32_t timeout = kernel_arg(3u);
    uint32_t result;
    if (alertable && deliver_user_apcs()) result = THREAD_STATUS_USER_APC;
    else if (timeout == 0u || *recomp_memory_u64(timeout) == 0u) {
        schedule();
        result = THREAD_STATUS_SUCCESS;
    } else result = wait_objects(0u, NULL, false, alertable != 0u, timeout);
    kernel_return(3u, result);
}

static void bridge_ke_initialize_timer_ex(void)
{
    uint32_t timer = kernel_arg(1u);
    uint32_t timer_type = kernel_arg(2u);

    if (timer != 0u) {
        uint32_t object_type = timer_type == 0u ? 0x08u : 0x09u;

        disarm_timer(timer);
        *recomp_memory_u32(timer) = object_type;
        *recomp_memory_u32(timer + DISPATCHER_SIGNAL_STATE) = 0u;
        *recomp_memory_u32(timer + 0x08u) = timer + 0x08u;
        *recomp_memory_u32(timer + 0x0cu) = timer + 0x08u;
        *recomp_memory_u64(timer + TIMER_DUE_TIME) = 0u;
        *recomp_memory_u32(timer + 0x18u) = 0u;
        *recomp_memory_u32(timer + 0x1cu) = 0u;
        *recomp_memory_u32(timer + TIMER_DPC) = 0u;
        *recomp_memory_u32(timer + 0x24u) = 0u;
    }
    kernel_return(2u, 0u);
}

static void bridge_ke_query_system_time(void)
{
    uint32_t current_time = kernel_arg(1u);

    if (current_time != 0u) *recomp_memory_u64(current_time) = system_time();
    kernel_return(1u, 0u);
}

static void bridge_ke_query_base_priority_thread(void)
{
    GuestThread *thread = find_thread(kernel_arg(1u));
    kernel_return(1u, thread == NULL ? 0u : (uint32_t)thread->base_increment);
}

/* KeQueryPerformanceCounter/Frequency take no arguments and return a
   ULONGLONG in edx:eax. They share the XAPI time model, so counter and
   frequency always agree. */
static void return_u64(uint64_t value)
{
    recomp_runtime.registers.edx = (uint32_t)(value >> 32);
    kernel_return(0u, (uint32_t)value);
}

static void bridge_ke_query_performance_counter(void)
{
    return_u64(recomp_xapi_performance_counter());
}

static void bridge_ke_query_performance_frequency(void)
{
    return_u64(recomp_xapi_performance_frequency());
}

/* KeQueryInterruptTime returns 100 ns units since boot. */
static void bridge_ke_query_interrupt_time(void)
{
    const uint64_t count = recomp_xapi_performance_counter();
    const uint64_t freq = recomp_xapi_performance_frequency();

    return_u64(count / freq * 10000000ull + count % freq * 10000000ull / freq);
}

static void bridge_ke_set_base_priority_thread(void)
{
    GuestThread *thread = find_thread(kernel_arg(1u));
    int32_t increment = (int32_t)kernel_arg(2u);
    int32_t previous = thread == NULL ? 0 : thread->base_increment;
    if (thread != NULL) {
        /* Xbox's normal process base is 8. XAPI maps idle/time-critical
           to saturated increments -16/+16; other results stay in 1..15. */
        thread->base_increment = increment <= -16 ? -16 : increment >= 16 ? 16
            : increment < -7 ? -7 : increment > 7 ? 7 : increment;
        thread->priority = increment < -7 ? 1 : increment > 7 ? 15 : 8 + increment;
    }
    kernel_return(2u, (uint32_t)previous);
    preempt_higher_priority();
}

static void bridge_ke_set_priority_thread(void)
{
    GuestThread *thread = find_thread(kernel_arg(1u));
    int32_t priority = (int32_t)kernel_arg(2u);
    int32_t previous = thread == NULL ? 0 : thread->priority;
    if (thread != NULL && priority >= 0 && priority <= 31) thread->priority = priority;
    kernel_return(2u, (uint32_t)previous);
    preempt_higher_priority();
}

static void bridge_ke_set_disable_boost_thread(void)
{
    kernel_return(2u, 0u);
}

static void bridge_ke_set_event(void)
{
    uint32_t event = kernel_arg(1u);
    uint32_t previous = 0u;

    if (event != 0u) {
        previous = *recomp_memory_u32(event + DISPATCHER_SIGNAL_STATE);
        *recomp_memory_u32(event + DISPATCHER_SIGNAL_STATE) = 1u;
    }
    kernel_return(3u, previous);
}

static void bridge_ke_set_timer(void)
{
    uint32_t timer = kernel_arg(1u);
    uint32_t dpc = kernel_arg(4u);
    uint32_t was_set = 0u;

    if (timer != 0u) {
        /* An overdue timer fires before it is replaced, as the clock interrupt would have. */
        expire_timer(timer);
        was_set = *recomp_memory_u64(timer + TIMER_DUE_TIME) != 0u;
        *recomp_memory_u32(timer + DISPATCHER_SIGNAL_STATE) = 0u;
        *recomp_memory_u64(timer + TIMER_DUE_TIME) =
            wait_deadline(recomp_runtime.registers.esp + 8u);
        *recomp_memory_u32(timer + TIMER_DPC) = dpc;
        if (dpc != 0u) arm_timer(timer);
    }
    kernel_return(4u, was_set);
}

static void bridge_ke_stall_execution_processor(void)
{
    kernel_return(1u, 0u);
}

static void bridge_ke_wait_for_single_object(void)
{
    uint32_t object = kernel_arg(1u);
    uint32_t result = wait_objects(1u, &object, false,
        kernel_arg(4u) != 0u, kernel_arg(5u));
    kernel_return(5u, result);
}

static void bridge_nt_create_event(void)
{
    uint32_t handle_pointer = kernel_arg(1u);
    SyntheticEvent *event;

    if (handle_pointer == 0u || kernel_arg(3u) > 1u) {
        kernel_return(4u, THREAD_STATUS_INVALID_PARAMETER);
        return;
    }
    event = create_event(kernel_arg(3u), kernel_arg(4u));
    if (event == NULL) {
        kernel_return(4u, THREAD_STATUS_INVALID_PARAMETER);
        return;
    }
    *recomp_memory_u32(handle_pointer) = event->handle;
    kernel_return(4u, THREAD_STATUS_SUCCESS);
}

static void change_suspend_count(bool suspend)
{
    GuestThread *thread = find_thread(kernel_arg(1u));
    uint32_t previous_pointer = kernel_arg(2u);
    if (thread == NULL || thread->exited) {
        kernel_return(2u, THREAD_STATUS_INVALID_HANDLE);
        return;
    }
    uint32_t previous = thread->suspend_count;
    if (suspend && previous == 127u) {
        kernel_return(2u, 0xc000004au);
        return;
    }
    if (suspend) ++thread->suspend_count;
    else if (previous != 0u) --thread->suspend_count;
    if (previous_pointer != 0u) *recomp_memory_u32(previous_pointer) = previous;
    kernel_return(2u, THREAD_STATUS_SUCCESS);
    if (suspend && thread == &threads[current_thread]) schedule();
    else if (!suspend) preempt_higher_priority();
}

static void bridge_nt_resume_thread(void)
{
    change_suspend_count(false);
}

static void bridge_ke_resume_thread(void)
{
    GuestThread *thread = find_thread(kernel_arg(1u));
    uint32_t previous = thread == NULL ? 0u : thread->suspend_count;
    if (thread != NULL && previous != 0u) --thread->suspend_count;
    kernel_return(1u, previous);
    preempt_higher_priority();
}

static void bridge_nt_set_event(void)
{
    uint32_t handle = kernel_arg(1u);
    uint32_t previous_pointer = kernel_arg(2u);
    SyntheticEvent *event = find_event(handle);
    uint32_t previous = event == NULL ? 0u : *recomp_memory_u32(event->object + 4u);

    if (event != NULL) {
        *recomp_memory_u32(event->object + 4u) = 1u;
    }
    if (previous_pointer != 0u) {
        *recomp_memory_u32(previous_pointer) = previous;
    }
    kernel_return(2u, event == NULL ? THREAD_STATUS_INVALID_HANDLE : THREAD_STATUS_SUCCESS);
}

static void bridge_nt_suspend_thread(void)
{
    change_suspend_count(true);
}

static void nt_wait_single(bool extended)
{
    uint32_t object = wait_object(kernel_arg(1u), true);
    uint32_t result = object == 0u ? THREAD_STATUS_INVALID_HANDLE :
        wait_objects(1u, &object, false, kernel_arg(extended ? 3u : 2u) != 0u,
                     kernel_arg(extended ? 4u : 3u));
    kernel_return(extended ? 4u : 3u, result);
}

static void bridge_nt_wait_for_single_object(void)
{
    nt_wait_single(false);
}

static void bridge_nt_wait_for_single_object_ex(void)
{
    nt_wait_single(true);
}

static void wait_multiple(bool handles)
{
    uint32_t count = kernel_arg(1u);
    uint32_t array = kernel_arg(2u);
    uint32_t type = kernel_arg(3u);
    uint32_t objects[MAX_EVENTS];
    uint32_t argc = handles ? 6u : 8u;
    if (count == 0u || count > MAX_EVENTS || type > 1u || array == 0u) {
        kernel_return(argc, THREAD_STATUS_INVALID_PARAMETER);
        return;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        objects[i] = wait_object(*recomp_memory_u32(array + i * 4u), handles);
        if (objects[i] == 0u) {
            kernel_return(argc, THREAD_STATUS_INVALID_HANDLE);
            return;
        }
    }
    uint32_t result = wait_objects(count, objects, type == 0u,
        kernel_arg(handles ? 5u : 6u) != 0u, kernel_arg(handles ? 6u : 7u));
    kernel_return(argc, result);
}

static void bridge_ke_wait_for_multiple_objects(void) { wait_multiple(false); }
static void bridge_nt_wait_for_multiple_objects_ex(void) { wait_multiple(true); }

/* NtUserIoApcDispatcher(ApcContext, IoStatusBlock, Reserved): XAPI's
   ReadFileEx/WriteFileEx APC. The context is the caller's completion routine,
   called as (error, bytes transferred, overlapped); the status block is the
   OVERLAPPED's first two fields. */
static void bridge_nt_user_io_apc_dispatcher(void)
{
    uint32_t routine = kernel_arg(1u);
    uint32_t io_status_block = kernel_arg(2u);
    uint32_t status = *recomp_memory_u32(io_status_block);
    /* ponytail: two DOS errors; use a full RtlNtStatusToDosError table if a
       completion routine branches on other codes. */
    uint32_t error = status < 0x80000000u ? 0u
        : status == 0xc0000011u ? 38u /* ERROR_HANDLE_EOF */
        : 31u;                        /* ERROR_GEN_FAILURE */
    const uint32_t arguments[3] = {
        error, error == 0u ? *recomp_memory_u32(io_status_block + 4u) : 0u,
        io_status_block,
    };

    kernel_call_guest(routine, arguments, 3u);
    kernel_return(3u, 0u);
}

static void bridge_nt_yield_execution(void)
{
    schedule();
    kernel_return(0u, THREAD_STATUS_SUCCESS);
}

static void bridge_ob_reference_object_by_handle(void)
{
    uint32_t handle = kernel_arg(1u);
    uint32_t object_pointer = kernel_arg(3u);

    uint32_t object = wait_object(handle, true);
    if (object_pointer != 0u) *recomp_memory_u32(object_pointer) = object;
    kernel_return(3u, object == 0u ? THREAD_STATUS_INVALID_HANDLE : THREAD_STATUS_SUCCESS);
}

/* ObfDereferenceObject is fastcall: the object arrives in ECX. */
static void bridge_obf_dereference_object(void)
{
    kernel_return_caller_cleanup(0u);
}

RecompFunction recomp_kernel_thread(uint32_t ordinal)
{
    switch (ordinal) {
    case 97u: return bridge_ke_cancel_timer;
    case 99u: return bridge_ke_delay_execution_thread;
    case 113u: return bridge_ke_initialize_timer_ex;
    case 124u: return bridge_ke_query_base_priority_thread;
    case 125u: return bridge_ke_query_interrupt_time;
    case 126u: return bridge_ke_query_performance_counter;
    case 127u: return bridge_ke_query_performance_frequency;
    case 128u: return bridge_ke_query_system_time;
    case 140u: return bridge_ke_resume_thread;
    case 143u: return bridge_ke_set_base_priority_thread;
    case 144u: return bridge_ke_set_disable_boost_thread;
    case 145u: return bridge_ke_set_event;
    case 148u: return bridge_ke_set_priority_thread;
    case 149u: return bridge_ke_set_timer;
    case 151u: return bridge_ke_stall_execution_processor;
    case 158u: return bridge_ke_wait_for_multiple_objects;
    case 159u: return bridge_ke_wait_for_single_object;
    case 189u: return bridge_nt_create_event;
    case 224u: return bridge_nt_resume_thread;
    case 225u: return bridge_nt_set_event;
    case 231u: return bridge_nt_suspend_thread;
    case 232u: return bridge_nt_user_io_apc_dispatcher;
    case 233u: return bridge_nt_wait_for_single_object;
    case 234u: return bridge_nt_wait_for_single_object_ex;
    case 235u: return bridge_nt_wait_for_multiple_objects_ex;
    case 238u: return bridge_nt_yield_execution;
    case 246u: return bridge_ob_reference_object_by_handle;
    case 255u: return bridge_ps_create_system_thread_ex;
    case 258u: return bridge_ps_terminate_system_thread;
    case 250u: return bridge_obf_dereference_object;
    default: return NULL;
    }
}
