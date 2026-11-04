#include "runtime.h"
#include "apu_model.h"
#include "ohci_model.h"
#include "stop_report.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

RecompRuntime recomp_runtime;
uint32_t recomp_last_dispatch_address;
/* Generated code pushes a literal return slot, so the guest stack cannot be
   walked. Dispatch is the only place a caller is known, so record the frames
   there and the innermost one names the function that faulted. */
static RecompDispatchFrame dispatch_stack[64];
static size_t dispatch_depth;
static uint8_t *direct_ram;
uint8_t *recomp_fast_ram;

static void report_dispatch_stack(void);
static uint8_t *find_direct_ram(uint32_t guest_address, size_t width);
static void watch_init(void);
static void update_fast_ram(void);

enum {
    XBOX_RAM_SIZE = RECOMP_XBOX_RAM_SIZE,
    XBOX_CACHED_ALIAS = RECOMP_XBOX_CACHED_ALIAS,
    XBOX_PHYSICAL_ALIAS = RECOMP_XBOX_PHYSICAL_ALIAS,
    USB0_OHCI_BASE = 0xfed00000u,
};

typedef struct RecompMmioU32Access {
    bool pending;
    uint32_t guest_address;
    RecompOhciRegister reg;
    uint32_t original;
    uint32_t value;
} RecompMmioU32Access;

static RecompOhci usb0_ohci;
static RecompApu mcpx_apu;
static RecompMmioU32Access mmio_u32_access;

void recomp_runtime_init(
    const RecompMemoryRegion *memory_regions,
    size_t memory_region_count,
    RecompMemoryAccess *accesses,
    size_t access_capacity,
    const RecompFunctionEntry *functions,
    size_t function_count)
{
    recomp_runtime = (RecompRuntime){
        .memory_regions = memory_regions,
        .memory_region_count = memory_region_count,
        .accesses = accesses,
        .access_capacity = access_capacity,
        .functions = functions,
        .function_count = function_count,
    };
    direct_ram = memory_region_count == 1u && memory_regions != NULL &&
        memory_regions[0].address == 0u &&
        memory_regions[0].size == 0x04000000u
        ? memory_regions[0].data : NULL;
    /* x87 reset default: all exceptions masked, round-to-nearest,
       extended precision. The guest CRT reads this back through FNSTCW. */
    recomp_runtime.fpu_control_word = 0x037fu;
    recomp_runtime.fpu_compare = 0;
    recomp_ohci_init(&usb0_ohci);
    recomp_apu_init(&mcpx_apu);
    mmio_u32_access = (RecompMmioU32Access){0};
    watch_init();
    update_fast_ram();
}

void recomp_runtime_set_lookup(RecompFunctionLookup lookup)
{
    recomp_runtime.lookup = lookup;
}

static void fail_memory_access(uint32_t guest_address, size_t width)
{
    ++recomp_runtime.undeclared_access_count;
    fprintf(
        stderr,
        "recomp runtime: undeclared %zu-byte guest memory access at 0x%08"
        PRIx32 "\n",
        width,
        guest_address);
    /* The faulting address is usually a base register plus a constant, so the
       registers say which pointer went bad and the last dispatch says where. */
    fprintf(
        stderr,
        "recomp runtime: undeclared access registers eax=0x%08" PRIx32
        " ecx=0x%08" PRIx32 " edx=0x%08" PRIx32 " ebx=0x%08" PRIx32
        " esi=0x%08" PRIx32 " edi=0x%08" PRIx32 " ebp=0x%08" PRIx32
        " esp=0x%08" PRIx32 " last-dispatch=0x%08" PRIx32 "\n",
        recomp_runtime.registers.eax,
        recomp_runtime.registers.ecx,
        recomp_runtime.registers.edx,
        recomp_runtime.registers.ebx,
        recomp_runtime.registers.esi,
        recomp_runtime.registers.edi,
        recomp_runtime.registers.ebp,
        recomp_runtime.registers.esp,
        recomp_last_dispatch_address);
    report_dispatch_stack();
#ifdef _WIN32
    if (getenv("RECOMP_FAULT_TRACE") != NULL) {
        void *frames[32];
        USHORT count = CaptureStackBackTrace(0u, 32u, frames, NULL);
        fprintf(stderr, "[DEBUG-r501-fault] module=%p frames=%u\n",
            (void *)GetModuleHandleW(NULL), (unsigned)count);
        for (USHORT i = 0u; i < count; ++i) {
            fprintf(stderr, "[DEBUG-r501-fault] frame=%u pc=%p\n",
                (unsigned)i, frames[i]);
        }
    }
#endif
    recomp_stop(2, "memory:0x%08" PRIx32, guest_address);
}

/* RECOMP_WATCHDOG_MS: for a run that goes quiet, sample the guest from a host
   thread after that many milliseconds and stop. The sample races the guest,
   so it is a hint. Return slots are literal guest addresses, so stack words
   that land in code name likely callers. */
#ifdef _WIN32
static DWORD WINAPI watchdog_thread(LPVOID parameter)
#else
static void *watchdog_thread(void *parameter)
#endif
{
    uint32_t esp;

#ifdef _WIN32
    Sleep((DWORD)(uintptr_t)parameter);
#else
    const unsigned long ms = (unsigned long)(uintptr_t)parameter;
    struct timespec wait = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&wait, &wait) != 0) {
    }
#endif
    esp = recomp_runtime.registers.esp;
    fprintf(stderr, "recomp watchdog: eax=0x%08" PRIx32 " ecx=0x%08" PRIx32
        " esi=0x%08" PRIx32 " edi=0x%08" PRIx32 " ebp=0x%08" PRIx32
        " esp=0x%08" PRIx32 " last-dispatch=0x%08" PRIx32 "\n",
        recomp_runtime.registers.eax, recomp_runtime.registers.ecx,
        recomp_runtime.registers.esi, recomp_runtime.registers.edi,
        recomp_runtime.registers.ebp, esp, recomp_last_dispatch_address);
    report_dispatch_stack();
    for (uint32_t i = 0u; i < 128u; ++i) {
        const uint32_t *slot = (const uint32_t *)(const void *)
            find_direct_ram(esp + i * 4u, sizeof(uint32_t));

        if (slot == NULL) {
            break;
        }
        fprintf(stderr, "recomp watchdog: stack+0x%03" PRIx32 "=0x%08" PRIx32
            "\n", i * 4u, *slot);
    }
    recomp_stop(2, "watchdog:%lu", (unsigned long)(uintptr_t)parameter);
    return 0;
}

void recomp_watchdog_start(void)
{
    const char *setting = getenv("RECOMP_WATCHDOG_MS");
    unsigned long ms = setting != NULL ? strtoul(setting, NULL, 10) : 0u;

    if (ms != 0u) {
#ifdef _WIN32
        CreateThread(NULL, 0, watchdog_thread, (LPVOID)(uintptr_t)ms, 0, NULL);
#else
        pthread_t thread;
        if (pthread_create(&thread, NULL, watchdog_thread, (void *)(uintptr_t)ms) == 0) {
            pthread_detach(thread);
        }
#endif
    }
}

static void record_memory_access(uint32_t guest_address, size_t width)
{
    if (recomp_runtime.accesses == NULL) {
        return;
    }
    if (recomp_runtime.access_count >= recomp_runtime.access_capacity) {
        fprintf(stderr, "recomp runtime: memory access log full\n");
        recomp_stop(2, "access-log-full");
    }

    recomp_runtime.accesses[recomp_runtime.access_count++] =
        (RecompMemoryAccess){
            .address = guest_address,
            .width = (uint32_t)width,
        };
}

/* MEM32 is an lvalue. Commit a changed staging word on the next memory access
   so device write semantics stay in the model rather than in interception. */
static void commit_mmio_u32_write(void)
{
    if (!mmio_u32_access.pending) {
        return;
    }

    mmio_u32_access.pending = false;
    update_fast_ram();
    if (mmio_u32_access.value == mmio_u32_access.original) {
        return;
    }
    if (!recomp_ohci_write(
            &usb0_ohci,
            mmio_u32_access.reg,
            mmio_u32_access.value)) {
        fprintf(
            stderr,
            "recomp runtime: unsupported OHCI write 0x%08" PRIx32
            " at 0x%08" PRIx32 "\n",
            mmio_u32_access.value,
            mmio_u32_access.guest_address);
        recomp_stop(
            2, "ohci-write:0x%08" PRIx32, mmio_u32_access.guest_address);
    }
}

static bool usb0_ohci_register(
    uint32_t guest_address,
    RecompOhciRegister *reg)
{
    switch (guest_address) {
    case USB0_OHCI_BASE + 0x00u:
        *reg = RECOMP_OHCI_HC_REVISION;
        return true;
    case USB0_OHCI_BASE + 0x04u:
        *reg = RECOMP_OHCI_HC_CONTROL;
        return true;
    case USB0_OHCI_BASE + 0x08u:
        *reg = RECOMP_OHCI_HC_COMMAND_STATUS;
        return true;
    case USB0_OHCI_BASE + 0x10u:
        *reg = RECOMP_OHCI_HC_INTERRUPT_ENABLE;
        return true;
    case USB0_OHCI_BASE + 0x18u:
        *reg = RECOMP_OHCI_HC_HCCA;
        return true;
    case USB0_OHCI_BASE + 0x1cu:
        *reg = RECOMP_OHCI_HC_PERIOD_CURRENT_ED;
        return true;
    case USB0_OHCI_BASE + 0x20u:
        *reg = RECOMP_OHCI_HC_CONTROL_HEAD_ED;
        return true;
    case USB0_OHCI_BASE + 0x24u:
        *reg = RECOMP_OHCI_HC_CONTROL_CURRENT_ED;
        return true;
    case USB0_OHCI_BASE + 0x28u:
        *reg = RECOMP_OHCI_HC_BULK_HEAD_ED;
        return true;
    case USB0_OHCI_BASE + 0x2cu:
        *reg = RECOMP_OHCI_HC_BULK_CURRENT_ED;
        return true;
    case USB0_OHCI_BASE + 0x30u:
        *reg = RECOMP_OHCI_HC_DONE_HEAD;
        return true;
    case USB0_OHCI_BASE + 0x34u:
        *reg = RECOMP_OHCI_HC_FM_INTERVAL;
        return true;
    case USB0_OHCI_BASE + 0x40u:
        *reg = RECOMP_OHCI_HC_PERIODIC_START;
        return true;
    case USB0_OHCI_BASE + 0x44u:
        *reg = RECOMP_OHCI_HC_LS_THRESHOLD;
        return true;
    case USB0_OHCI_BASE + 0x48u:
        *reg = RECOMP_OHCI_HC_RH_DESCRIPTOR_A;
        return true;
    case USB0_OHCI_BASE + 0x4cu:
        *reg = RECOMP_OHCI_HC_RH_DESCRIPTOR_B;
        return true;
    case USB0_OHCI_BASE + 0x50u:
        *reg = RECOMP_OHCI_HC_RH_STATUS;
        return true;
    case USB0_OHCI_BASE + 0x54u:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_1;
        return true;
    case USB0_OHCI_BASE + 0x58u:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_2;
        return true;
    case USB0_OHCI_BASE + 0x5cu:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_3;
        return true;
    case USB0_OHCI_BASE + 0x60u:
        *reg = RECOMP_OHCI_HC_RH_PORT_STATUS_4;
        return true;
    default:
        return false;
    }
}

static uint8_t *find_memory_region(uint32_t guest_address, size_t width)
{
    uint64_t access_end = (uint64_t)guest_address + width;

    for (size_t i = 0; i < recomp_runtime.memory_region_count; ++i) {
        const RecompMemoryRegion *region = &recomp_runtime.memory_regions[i];
        uint64_t region_end = (uint64_t)region->address + region->size;

        if (region->data != NULL &&
            guest_address >= region->address &&
            access_end <= region_end) {
            return region->data + (guest_address - region->address);
        }
    }

    return NULL;
}

/* Round 37 write-watch. Every guest memory access in generated code funnels
   through recomp_memory(), so polling one watched word here catches its next
   writer without a hardware breakpoint or a host debugger. The report names
   the writer through the dispatch stack, which carries __FILE__/__LINE__
   because the lifted guest stack does not carry a usable return address.
   Env-gated: RECOMP_WATCH is a hex guest address, and the watch is inert
   when it is unset. */
static struct {
    bool active;
    bool armed;
    uint32_t address;
    uint32_t value;
    uint32_t hits;
    uint32_t hit_limit;
} watch;

static void update_fast_ram(void)
{
    recomp_fast_ram = !watch.active && recomp_runtime.accesses == NULL &&
        !mmio_u32_access.pending ? direct_ram : NULL;
}

static void watch_init(void)
{
    const char *setting = getenv("RECOMP_WATCH");
    const char *limit;

    watch.active = false;
    if (setting == NULL || setting[0] == '\0') {
        return;
    }
    watch.address = (uint32_t)strtoul(setting, NULL, 16);
    if (watch.address == 0u) {
        return;
    }
    limit = getenv("RECOMP_WATCH_HITS");
    watch.hit_limit = limit != NULL ? (uint32_t)strtoul(limit, NULL, 10) : 40u;
    watch.active = true;
    watch.armed = false;
    watch.hits = 0u;
    fprintf(
        stderr,
        "recomp watch: watching 0x%08" PRIx32 " limit=%" PRIu32 "\n",
        watch.address,
        watch.hit_limit);
}

static void report_watch_writer(uint32_t previous, uint32_t current)
{
    fprintf(stderr, "recomp watch: hit %" PRIu32 " at 0x%08" PRIx32
        " 0x%08" PRIx32 " -> 0x%08" PRIx32 " last-dispatch=0x%08" PRIx32 "\n",
        watch.hits, watch.address, previous, current, recomp_last_dispatch_address);
    report_dispatch_stack();
}

static void check_watch_write(void)
{
    const uint8_t *data;
    uint32_t current;

    if (!watch.active) {
        return;
    }

    data = find_memory_region(watch.address, sizeof(uint32_t));
    if (data == NULL) {
        return;
    }
    memcpy(&current, data, sizeof current);
    if (!watch.armed) {
        watch.armed = true;
        watch.value = current;
        return;
    }
    if (current == watch.value) {
        return;
    }

    ++watch.hits;
    if (watch.hits <= watch.hit_limit) {
        report_watch_writer(watch.value, current);
    } else if (watch.hits == watch.hit_limit + 1u) {
        fprintf(stderr, "recomp watch: hit limit reached, silencing\n");
    }
    watch.value = current;
}

/* The full runner maps one contiguous RAM allocation. Keep instrumented and
   device accesses on the general path, including the preceding OHCI flush. */
static uint8_t *find_direct_ram(uint32_t guest_address, size_t width)
{
    uint32_t window = guest_address & 0xfc000000u;
    uint32_t offset = guest_address & 0x03ffffffu;

    if (direct_ram == NULL || watch.active || recomp_runtime.accesses != NULL ||
        (window != 0u && window != XBOX_CACHED_ALIAS &&
         window != XBOX_PHYSICAL_ALIAS) || width > XBOX_RAM_SIZE - offset) {
        return NULL;
    }
    return direct_ram + offset;
}

/* Keep device and diagnostic work out of the inlined RAM accessors. */
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static uint8_t *recomp_memory_slow(uint32_t guest_address, size_t width)
{
    uint8_t *memory;

    check_watch_write();

    /* MCPX audio aperture is device space, not guest RAM, so it is served
       from its model instead of a declared region. */
    memory = recomp_apu_access(&mcpx_apu, guest_address, width);
    if (memory != NULL) {
        record_memory_access(guest_address, width);
        return memory;
    }

    memory = find_memory_region(guest_address, width);
    if (memory == NULL && guest_address >= XBOX_CACHED_ALIAS &&
        guest_address - XBOX_CACHED_ALIAS < XBOX_RAM_SIZE) {
        memory = find_memory_region(
            guest_address - XBOX_CACHED_ALIAS, width);
    }
    /* Xbox D3D returns this physical-memory view for D3DLOCK_TILED. */
    if (memory == NULL && guest_address >= XBOX_PHYSICAL_ALIAS &&
        guest_address - XBOX_PHYSICAL_ALIAS < XBOX_RAM_SIZE) {
        memory = find_memory_region(
            guest_address - XBOX_PHYSICAL_ALIAS, width);
    }
    if (memory != NULL) {
        record_memory_access(guest_address, width);
        return memory;
    }

    fail_memory_access(guest_address, width);
    return NULL;
}

uint8_t *recomp_memory(uint32_t guest_address, size_t width)
{
    uint8_t *memory;

    if (mmio_u32_access.pending) {
        commit_mmio_u32_write();
    }
    memory = find_direct_ram(guest_address, width);
    if (memory != NULL) {
        return memory;
    }
    return recomp_memory_slow(guest_address, width);
}

uint32_t *recomp_memory_u32_checked(uint32_t guest_address)
{
    RecompOhciRegister reg;
    uint32_t value;
    uint8_t *memory;

    if (mmio_u32_access.pending) {
        commit_mmio_u32_write();
    }
    memory = find_direct_ram(guest_address, sizeof(uint32_t));
    if (memory != NULL) {
        return (uint32_t *)(void *)memory;
    }
    if (usb0_ohci_register(guest_address, &reg)) {
        if (!recomp_ohci_read(&usb0_ohci, reg, &value)) {
            fail_memory_access(guest_address, sizeof(uint32_t));
            return NULL;
        }
        record_memory_access(guest_address, sizeof(uint32_t));
        mmio_u32_access = (RecompMmioU32Access){
            .pending = true,
            .guest_address = guest_address,
            .reg = reg,
            .original = value,
            .value = value,
        };
        update_fast_ram();
        return &mmio_u32_access.value;
    }

    return (uint32_t *)(void *)recomp_memory(
        guest_address, sizeof(uint32_t));
}

void recomp_guest_memcpy(
    uint32_t destination,
    uint32_t source,
    size_t size)
{
    uint8_t *destination_data;
    const uint8_t *source_data;

    if (size == 0u) {
        return;
    }

    source_data = recomp_memory(source, size);
    destination_data = recomp_memory(destination, size);
    memcpy(destination_data, source_data, size);
}

void recomp_guest_memset(
    uint32_t destination,
    int value,
    size_t size)
{
    uint8_t *destination_data;

    if (size == 0u) {
        return;
    }

    destination_data = recomp_memory(destination, size);
    memset(destination_data, value, size);
}

void recomp_guest_load(void *destination, uint32_t source, size_t size)
{
    const uint8_t *source_data;

    if (size == 0u) {
        return;
    }

    source_data = recomp_memory(source, size);
    memcpy(destination, source_data, size);
}

void recomp_guest_store(uint32_t destination, const void *source, size_t size)
{
    uint8_t *destination_data;

    if (size == 0u) {
        return;
    }

    destination_data = recomp_memory(destination, size);
    memcpy(destination_data, source, size);
}

uint32_t *recomp_ebp_register(void)
{
    return &recomp_runtime.registers.ebp;
}

uint32_t *recomp_esp_register(void)
{
    return &recomp_runtime.registers.esp;
}

bool recomp_dispatch(uint32_t guest_address)
{
    /* ponytail: linear lookup; sort and binary-search when table size matters. */
    for (size_t i = 0; i < recomp_runtime.function_count; ++i) {
        const RecompFunctionEntry *entry = &recomp_runtime.functions[i];

        if (entry->address == guest_address && entry->function != NULL) {
            recomp_last_dispatch_address = guest_address;
            entry->function();
            return true;
        }
    }

    if (recomp_runtime.lookup != NULL) {
        RecompFunction function = recomp_runtime.lookup(guest_address);

        if (function != NULL) {
            recomp_last_dispatch_address = guest_address;
            function();
            return true;
        }
    }

    return false;
}

static const char *member_name(const char *member)
{
    const char *name = member;

    for (const char *cursor = member; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            name = cursor + 1;
        }
    }
    return name;
}

static void copy_token(char *out, size_t size, const char *text)
{
    size_t i = 0;

    while (i + 1u < size && text[i] != '\0' && text[i] != ':' &&
           text[i] != '(' && text[i] != ' ' && text[i] != '\r' &&
           text[i] != '\n') {
        out[i] = text[i];
        ++i;
    }
    out[i] = '\0';
}

/* Names the generated site by rescanning the compiled member for the last
   guest label above the reported line; generated members carry no line map. */
static void generated_site(
    const char *member,
    int line,
    char *function,
    size_t function_size,
    char *label,
    size_t label_size)
{
    FILE *source = fopen(member, "rb");
    char text[512];
    int line_number = 1;
    int at_line_start = 1;

    function[0] = '\0';
    label[0] = '\0';
    if (source == NULL) {
        return;
    }

    while (line_number <= line && fgets(text, sizeof text, source) != NULL) {
        size_t length;

        if (at_line_start) {
            if (strncmp(text, "loc_", 4) == 0) {
                copy_token(label, label_size, text);
            } else if (strncmp(text, "void sub_", 9) == 0) {
                copy_token(function, function_size, text + 5);
                label[0] = '\0';
            }
        }
        length = strlen(text);
        at_line_start = length > 0u && text[length - 1u] == '\n';
        if (at_line_start) {
            ++line_number;
        }
    }
    fclose(source);
}

void recomp_generated_breakpoint(const char *member, int line)
{
    char function[64];
    char label[64];

    generated_site(member, line, function, sizeof function, label, sizeof label);
    fprintf(
        stderr,
        "recomp runtime: generated breakpoint at %s:%d function=%s label=%s"
        " esp=0x%08" PRIx32 " ebp=0x%08" PRIx32 " dispatch=0x%08" PRIx32 "\n",
        member_name(member),
        line,
        function[0] != '\0' ? function : "<unknown>",
        label[0] != '\0' ? label : "<unknown>",
        recomp_runtime.registers.esp,
        recomp_runtime.registers.ebp,
        recomp_last_dispatch_address);
    recomp_stop(2, "breakpoint:%s", member_name(member));
}

void recomp_unimpl(const char *text, uint32_t va)
{
    fprintf(stderr, "recomp runtime: untranslated instruction '%s' at 0x%08"
        PRIx32 "\n", text, va);
    recomp_stop(2, "unimpl:0x%08" PRIx32, va);
}

/* Innermost frame first: the top entry is the function that faulted, and the
   rest is the guest call path that reached it. */
static void report_dispatch_stack(void)
{
    size_t capacity = sizeof dispatch_stack / sizeof dispatch_stack[0];
    size_t depth = dispatch_depth < capacity ? dispatch_depth : capacity;
    size_t reported = 0;

    while (depth > 0u && reported < 12u) {
        const RecompDispatchFrame *frame = &dispatch_stack[--depth];
        char function[64];
        char label[64];

        if (frame->member == NULL) {
            fprintf(
                stderr,
                "recomp runtime: dispatch frame %zu target=0x%08" PRIx32
                " site=<adapter>\n",
                reported,
                frame->guest_address);
            ++reported;
            continue;
        }
        generated_site(
            frame->member,
            frame->line,
            function,
            sizeof function,
            label,
            sizeof label);
        fprintf(
            stderr,
            "recomp runtime: dispatch frame %zu target=0x%08" PRIx32
            " site=%s:%d function=%s label=%s\n",
            reported,
            frame->guest_address,
            member_name(frame->member),
            frame->line,
            function[0] != '\0' ? function : "<unknown>",
            label[0] != '\0' ? label : "<unknown>");
        ++reported;
    }
}

static void recomp_dispatch_indirect_from(
    uint32_t guest_address,
    uint32_t saved_esp,
    const char *member,
    int line)
{
    if (dispatch_depth < sizeof dispatch_stack / sizeof dispatch_stack[0]) {
        dispatch_stack[dispatch_depth] = (RecompDispatchFrame){
            .guest_address = guest_address,
            .member = member,
            .line = line,
        };
    }
    ++dispatch_depth;
    if (recomp_dispatch(guest_address)) {
        --dispatch_depth;
        return;
    }
    --dispatch_depth;

    /* Encoded kernel thunks read as 0x8000xxxx; name the missing import. */
    if ((guest_address & 0xffff0000u) == 0x80000000u) {
        uint32_t ordinal = guest_address & 0xffffu;
        const char *name = recomp_kernel_ordinal_name(ordinal);

        fprintf(
            stderr,
            "recomp runtime: unimplemented kernel import ordinal %" PRIu32
            " (%s) at saved esp 0x%08" PRIx32 "\n",
            ordinal,
            name != NULL ? name : "name unavailable",
            saved_esp);
        recomp_stop(
            2, "import:%s", name != NULL ? name : "unknown");
    }

    fprintf(
        stderr,
        "recomp runtime: unresolved indirect call to 0x%08" PRIx32
        " at saved esp 0x%08" PRIx32 "\n",
        guest_address,
        saved_esp);
    fprintf(
        stderr,
        "recomp runtime: unresolved indirect registers eax=0x%08" PRIx32
        " ecx=0x%08" PRIx32 " edx=0x%08" PRIx32
        " ebx=0x%08" PRIx32 " esi=0x%08" PRIx32
        " edi=0x%08" PRIx32 "\n",
        recomp_runtime.registers.eax,
        recomp_runtime.registers.ecx,
        recomp_runtime.registers.edx,
        recomp_runtime.registers.ebx,
        recomp_runtime.registers.esi,
        recomp_runtime.registers.edi);
    if (member != NULL) {
        fprintf(
            stderr,
            "recomp runtime: unresolved indirect site %s:%d\n",
            member_name(member),
            line);
    }
    recomp_stop(2, "indirect:0x%08" PRIx32, guest_address);
}

void recomp_dispatch_indirect(uint32_t guest_address, uint32_t saved_esp)
{
    recomp_dispatch_indirect_from(guest_address, saved_esp, NULL, 0);
}

void recomp_dispatch_indirect_site(
    uint32_t guest_address,
    uint32_t saved_esp,
    const char *member,
    int line)
{
    recomp_dispatch_indirect_from(guest_address, saved_esp, member, line);
}

bool recomp_dispatch_frame_at(
    size_t depth,
    uint32_t *guest_address,
    const char **member,
    int *line)
{
    size_t capacity = sizeof dispatch_stack / sizeof dispatch_stack[0];
    size_t live = dispatch_depth < capacity ? dispatch_depth : capacity;
    const RecompDispatchFrame *frame;

    if (depth >= live) {
        return false;
    }
    /* Depth 0 is the innermost frame, matching how a caller thinks about a
       backtrace, while the array grows outward from index 0. */
    frame = &dispatch_stack[live - 1u - depth];
    if (guest_address != NULL) {
        *guest_address = frame->guest_address;
    }
    if (member != NULL) {
        *member = frame->member;
    }
    if (line != NULL) {
        *line = frame->line;
    }
    return true;
}

void recomp_dispatch_context_save(RecompDispatchContext *context)
{
    memcpy(context->frames, dispatch_stack, sizeof dispatch_stack);
    context->depth = dispatch_depth;
    context->last_address = recomp_last_dispatch_address;
}

void recomp_dispatch_context_clear(void)
{
    dispatch_depth = 0u;
    recomp_last_dispatch_address = 0u;
}

void recomp_dispatch_context_restore(const RecompDispatchContext *context)
{
    memcpy(dispatch_stack, context->frames, sizeof dispatch_stack);
    dispatch_depth = context->depth;
    recomp_last_dispatch_address = context->last_address;
}

void recomp_fpu_context_save(RecompFpuContext *context)
{
    if (context == NULL) {
        return;
    }
    memcpy(context->mmx, recomp_runtime.mmx, sizeof context->mmx);
    context->fpu_status_cc = recomp_runtime.fpu_status_cc;
    context->direction_flag = recomp_runtime.direction_flag;
    context->arithmetic_flags = recomp_runtime.arithmetic_flags;
    memcpy(context->xmm, recomp_runtime.xmm, sizeof context->xmm);
    memcpy(
        context->fpu_stack,
        recomp_runtime.fpu_stack,
        sizeof context->fpu_stack);
    context->fpu_top = recomp_runtime.fpu_top;
    context->fpu_control_word = recomp_runtime.fpu_control_word;
    context->fpu_compare = recomp_runtime.fpu_compare;
}

void recomp_fpu_context_restore(const RecompFpuContext *context)
{
    if (context == NULL) {
        return;
    }
    memcpy(recomp_runtime.mmx, context->mmx, sizeof recomp_runtime.mmx);
    recomp_runtime.fpu_status_cc = context->fpu_status_cc;
    recomp_runtime.direction_flag = context->direction_flag;
    recomp_runtime.arithmetic_flags = context->arithmetic_flags;
    memcpy(recomp_runtime.xmm, context->xmm, sizeof recomp_runtime.xmm);
    memcpy(
        recomp_runtime.fpu_stack,
        context->fpu_stack,
        sizeof recomp_runtime.fpu_stack);
    recomp_runtime.fpu_top = context->fpu_top;
    recomp_runtime.fpu_control_word = context->fpu_control_word;
    recomp_runtime.fpu_compare = context->fpu_compare;
}
