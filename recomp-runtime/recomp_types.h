#ifndef DOAXBV_RECOMP_TYPES_H
#define DOAXBV_RECOMP_TYPES_H

#define RECOMP_RUNTIME_NO_GENERATED_MACROS
#include "runtime.h"
#include "xbox_memory_layout.h"
#undef RECOMP_RUNTIME_NO_GENERATED_MACROS

#include <math.h>
#include <string.h>
/* Tests define RECOMP_HOST_X86=0 on x86 to check the portable lane code. */
#ifndef RECOMP_HOST_X86
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define RECOMP_HOST_X86 1
#else
#define RECOMP_HOST_X86 0
#endif
#endif
#if RECOMP_HOST_X86
#include <xmmintrin.h>
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

static inline uint32_t BSWAP32(uint32_t value)
{
    return (value >> 24) | ((value >> 8) & 0x0000ff00u) |
           ((value << 8) & 0x00ff0000u) | (value << 24);
}

/* Generated INT3 sites report their guest site instead of trapping the host. */
#undef __debugbreak
#define __debugbreak() recomp_generated_breakpoint(__FILE__, __LINE__)

typedef RecompFunction recomp_func_t;

recomp_func_t recomp_lookup(uint32_t guest_address);
recomp_func_t recomp_lookup_manual(uint32_t guest_address);
recomp_func_t recomp_lookup_kernel(uint32_t guest_address);
void recomp_debug_service(uint32_t service, uint32_t arg_va);


#define eax recomp_runtime.registers.eax
#define ecx recomp_runtime.registers.ecx
#define edx recomp_runtime.registers.edx
#define ebx recomp_runtime.registers.ebx
#define esi recomp_runtime.registers.esi
#define edi recomp_runtime.registers.edi
#define esp recomp_runtime.registers.esp
#define g_esp (*recomp_esp_register())
#define g_seh_ebp recomp_runtime.registers.ebp
/* The lifter publishes the established frame as g_ebp and re-publishes it
   before calls; g_seh_ebp is the same guest EBP the SEH helper path reads.
   One register, two emitted names. */
#define g_ebp recomp_runtime.registers.ebp
#define g_fp_stack recomp_runtime.fpu_stack
#define g_fp_top recomp_runtime.fpu_top
#define g_fp_control_word recomp_runtime.fpu_control_word
#define g_fp_cmp recomp_runtime.fpu_compare
#define g_fp_cc recomp_runtime.fpu_status_cc
#define g_df recomp_runtime.direction_flag
#define g_eflags recomp_runtime.arithmetic_flags
/* FS state stays outside guest physical allocations, including page zero. */
#define XBOX_FS_BASE XBOX_PCR_BASE

#define RECOMP_ABI_CALL(va, fn) do { \
    RecompFunction manual = recomp_lookup_manual((uint32_t)(va)); \
    if (manual != NULL) manual(); \
    else (fn)(); \
} while (0)
#define RECOMP_ICALL_SAFE_AT(address, saved_esp, site) \
    RECOMP_ICALL_SAFE(address, saved_esp)

/* String-instruction step under EFLAGS.DF. */
#define RECOMP_DF_STEP(n) (g_df ? -(int32_t)(n) : (int32_t)(n))

/* Locked read-modify-write. Guest code runs on one host thread, so a plain
   read-compare-write on guest memory is atomic with respect to the guest. */
static inline uint32_t recomp_atomic_add32(uint32_t address, uint32_t value)
{
    uint32_t *slot = recomp_memory_u32(address);
    uint32_t old = *slot;

    *slot = old + value;
    return old;
}
#define RECOMP_ATOMIC_ADD32(pointer, value) \
    recomp_atomic_add32((uint32_t)(uintptr_t)(pointer), (uint32_t)(value))

static inline uint32_t recomp_atomic_cas32(
    uint32_t address, uint32_t compare, uint32_t value)
{
    uint32_t *slot = recomp_memory_u32(address);
    uint32_t old = *slot;

    if (old == compare) {
        *slot = value;
    }
    return old;
}
#define RECOMP_ATOMIC_CAS32(pointer, compare, value) \
    recomp_atomic_cas32((uint32_t)(uintptr_t)(pointer), \
                        (uint32_t)(compare), (uint32_t)(value))

/* Stop on every reached untranslated instruction during initial bring-up. */
void recomp_unimpl(const char *text, uint32_t va);
#define RECOMP_UNIMPL(text, va) recomp_unimpl((text), (va))

/* rdtsc at the Xbox CPU clock, 733.33 MHz. */
uint64_t xbox_ReadTimeStampCounter(void);

/* x87 precision control (control word bits 8-9). PC=24 rounds every
   arithmetic result to float precision while keeping the register's
   exponent range. Ported from the lifter's runtime template. */
static inline double recomp_fp_round24(double x)
{
    double ax = fabs(x);
    int e;

    if ((ax <= 3.4028234663852886e38 && ax >= 1.1754943508222875e-38) ||
        ax == 0.0 || x != x || ax == INFINITY) {
        return (double)(float)x;
    }
    x = frexp(x, &e);
    return ldexp((double)(float)x, e);
}
#define RECOMP_FP_PC(x) \
    ((g_fp_control_word & 0x300u) ? (double)(x) : recomp_fp_round24(x))
/* RECOMP_FCMP result as status-word C3/C2/C0. */
#define RECOMP_FCMP_CC(c) ((uint16_t)((c) == 2 ? 0x4500u : (c) < 0 ? 0x0100u \
                                      : (c) > 0 ? 0u : 0x4000u))

static inline uint16_t recomp_fxam(double value)
{
    return (uint16_t)((signbit(value) ? 0x0200u : 0u) |
        (isnan(value) ? 0x0100u : isinf(value) ? 0x0500u :
         value == 0.0 ? 0x4000u : 0x0400u));
}

/* FRNDINT under the guest RC bits (control word bits 10-11), which the
   host rounding mode never sees. */
static inline double recomp_frndint(double value, uint16_t control)
{
    double rounded;

    if (!isfinite(value)) {
        return value;
    }
    switch ((control >> 10) & 3) {
    case 1: rounded = floor(value); break;
    case 2: rounded = ceil(value); break;
    case 3: rounded = trunc(value); break;
    default: {
        double low = floor(value);
        double fraction = value - low;

        rounded = low;
        if (fraction > 0.5 || (fraction == 0.5 && fmod(low, 2.0) != 0.0)) {
            rounded = low + 1.0;
        }
        break;
    }
    }
    return rounded == 0.0 ? copysign(0.0, value) : rounded;
}

/* FIST: guest rounding; out of range stores the integer-indefinite value. */
static inline int64_t recomp_fist(double value, uint16_t control, unsigned bits)
{
    double rounded = recomp_frndint(value, control);
    double limit = ldexp(1.0, (int)bits - 1);

    if (!isfinite(rounded) || rounded < -limit || rounded >= limit) {
        return bits == 64u ? INT64_MIN : -(INT64_C(1) << (bits - 1u));
    }
    return (int64_t)rounded;
}

/* FLD/FSTP tbyte: 64-bit significand with an explicit integer bit, 15-bit
   exponent biased by 16383. The CRT's libm loads its constants this way.
   Ported from the lifter's runtime template. */
static inline double recomp_f80_load(uint64_t mant, uint16_t se)
{
    int e = se & 0x7fff;
    double v = e == 0x7fff ? ((mant << 1) ? NAN : INFINITY)
                           : ldexp((double)mant, e - 16383 - 63);

    return (se & 0x8000u) ? -v : v;
}

static inline uint16_t recomp_f80_store(double v, uint64_t *mant)
{
    union { double d; uint64_t u; } bits;
    uint16_t sign = signbit(v) ? 0x8000u : 0u;
    int e;

    bits.d = v;
    if (v != v) {
        *mant = 0x8000000000000000ull | (bits.u << 11);
        return sign | 0x7fffu;
    }
    if (isinf(v)) {
        *mant = 0x8000000000000000ull;
        return sign | 0x7fffu;
    }
    if (v == 0.0) {
        *mant = 0;
        return sign;
    }
    v = frexp(fabs(v), &e);
    *mant = (uint64_t)(int64_t)ldexp(v, 63) << 1;
    return (uint16_t)(sign | (e + 16382));
}

/* FSIN/FCOS/FSINCOS/FPTAN: an operand of 2^63 or more sets C2 and leaves
   the value and the stack depth unchanged. */
static inline int recomp_fp_trig_in_range(double value)
{
    if (isfinite(value) && fabs(value) >= 9223372036854775808.0) {
        g_fp_cc |= 0x0400u;
        return 0;
    }
    g_fp_cc &= (uint16_t)~0x0400u;
    return 1;
}

/* FSCALE: scale chopped toward zero; clamped so the cast stays defined. */
static inline double recomp_fscale(double value, double scale)
{
    if (isnan(value) || isnan(scale)) {
        return value + scale;
    }
    if (scale == INFINITY) {
        return value == 0.0 ? NAN : copysign(INFINITY, value);
    }
    if (scale == -INFINITY) {
        return isinf(value) ? NAN : copysign(0.0, value);
    }
    return scalbn(value,
        scale > 4096 ? 4096 : scale < -4096 ? -4096 : (int)scale);
}

/* FPREM (chop) and FPREM1 (nearest). C2 stays set while the reduction is
   partial; once complete, C0/C3/C1 carry quotient bits 2/1/0. */
static inline double recomp_fprem(double a, double b, int nearest)
{
    int quotient = 0, ea = 0, eb = 0;
    unsigned q;
    double result;

    if (isfinite(a) && isfinite(b) && a != 0.0 && b != 0.0) {
        frexp(a, &ea);
        frexp(b, &eb);
        if (ea - eb >= 64) {
            g_fp_cc |= 0x0400u;
            return fmod(a, scalbn(b, ea - eb - 32));
        }
    }
    result = remquo(a, b, &quotient);
    q = (unsigned)(quotient < 0 ? -quotient : quotient);
    if (!nearest && result != 0.0 && isfinite(result) &&
        signbit(result) != signbit(a)) {
        result += copysign(fabs(b), a);
        q -= 1u;
    }
    g_fp_cc = (uint16_t)(((q & 4u) << 6) | ((q & 2u) << 13) |
                         ((q & 1u) << 9));
    return result;
}

/* Result of an x87 compare, in the shape the status word wants:
 *   -1 less, 0 equal, 1 greater, 2 unordered (either operand is NaN).
 * The unordered case matters: `fucompp` of a value with itself followed by
 * `test ah, 0x44; jp` is how this era's CRT asks "is this a NaN", and
 * collapsing it to "equal" answers no every time.
 * Ported from the lifter's own runtime template. */
#define RECOMP_FCMP(a, b) \
    (((a) != (a) || (b) != (b)) ? 2 : (a) < (b) ? -1 : (a) > (b) ? 1 : 0)

/* x86 parity flag: 1 when the low byte of the result has an EVEN number of set
 * bits. Used by the x87 float-compare idiom `fnstsw ax; test ah, mask; jp/jnp`,
 * which is how all pre-SSE code branches on a float comparison.
 * Ported from the lifter's own runtime template. */
static inline int recomp_parity8(uint32_t x)
{
    x &= 0xFFu; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return (int)(~x & 1u);   /* 1 = even parity (PF set) */
}
#define RECOMP_PARITY8(x) recomp_parity8((uint32_t)(x))

#define MEM32(address) (*recomp_memory_u32((uint32_t)(address)))
#define MEM16(address) (*recomp_memory_u16((uint32_t)(address)))
#define MEM8(address) \
    (*(uint8_t *)(void *)recomp_memory_i8((uint32_t)(address)))
#define SMEM32(address) \
    (*(int32_t *)(void *)recomp_memory_u32((uint32_t)(address)))
#define SMEM64(address) \
    (*(int64_t *)(void *)recomp_memory_u64((uint32_t)(address)))
#define SMEM16(address) \
    (*(int16_t *)(void *)recomp_memory_u16((uint32_t)(address)))
#define SMEM8(address) (*recomp_memory_i8((uint32_t)(address)))
#define MEMF(address) \
    (*(float *)(void *)recomp_memory_u32((uint32_t)(address)))
#define MEMD(address) \
    (*(double *)(void *)recomp_memory_u64((uint32_t)(address)))
#define XBOX_PTR(address) ((void *)(uintptr_t)(uint32_t)(address))
#undef memcpy
#define memcpy(destination, source, size) \
    recomp_guest_memcpy( \
        (uint32_t)(uintptr_t)(destination), \
        (uint32_t)(uintptr_t)(source), \
        (size_t)(size))
#undef memset
#define memset(destination, value, size) \
    recomp_guest_memset( \
        (uint32_t)(uintptr_t)(destination), \
        (int)(value), \
        (size_t)(size))

#define ZX8(value) ((uint32_t)(uint8_t)(value))
#define ZX16(value) ((uint32_t)(uint16_t)(value))
#define SX8(value) ((uint32_t)(int32_t)(int8_t)(uint8_t)(value))
#define SX16(value) ((uint32_t)(int32_t)(int16_t)(uint16_t)(value))
#define LO8(value) ((uint8_t)((value) & 0xffu))
#define HI8(value) ((uint8_t)(((value) >> 8) & 0xffu))
/* x86 PF: set when the low byte of the result has an even number of set
   bits. The CRT branches on it after FNSTSW AX; TEST AH, mask. */
#define PARITY8(value) \
    ((((0x9669u >> (((uint8_t)(value) ^ ((uint8_t)(value) >> 4)) & 0xfu)) \
       & 1u)) != 0u)
#define LO16(value) ((uint16_t)((value) & 0xffffu))
#define SET_HI8(value, high) \
    ((value) = ((value) & 0xffff00ffu) | \
               ((uint32_t)(uint8_t)(high) << 8))
#define SET_LO8(value, low) \
    ((value) = ((value) & 0xffffff00u) | (uint32_t)(uint8_t)(low))
#define SET_LO16(value, low) \
    ((value) = ((value) & 0xffff0000u) | ((uint32_t)(uint16_t)(low)))

#define CMP_EQ(a, b) ((uint32_t)(a) == (uint32_t)(b))
#define CMP_NE(a, b) ((uint32_t)(a) != (uint32_t)(b))
#define CMP_B(a, b) ((uint32_t)(a) < (uint32_t)(b))
#define CMP_AE(a, b) ((uint32_t)(a) >= (uint32_t)(b))
#define CMP_BE(a, b) ((uint32_t)(a) <= (uint32_t)(b))
#define CMP_A(a, b) ((uint32_t)(a) > (uint32_t)(b))
/* x86 takes SF from the top bit of the operand width, not from bit 31. The
   generated code hands LO8/HI8/LO16 sub-register reads straight to these
   macros, so recover the operand width and sign-extend at that width. A
   sub-register read zero-extends, so casting it to int32_t would make every
   signed test read as non-negative. */
#define RECOMP_FLAG_WIDTH(a, b) (sizeof(a) < sizeof(b) ? sizeof(a) : sizeof(b))
#define RECOMP_SIGNED(value, width) \
    ((width) == 1u ? (int32_t)(int8_t)(uint8_t)(uint32_t)(value) \
     : (width) == 2u ? (int32_t)(int16_t)(uint16_t)(uint32_t)(value) \
     : (int32_t)(uint32_t)(value))
#define CMP_L(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) < \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_GE(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) >= \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_LE(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) <= \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define CMP_G(a, b) \
    (RECOMP_SIGNED(a, RECOMP_FLAG_WIDTH(a, b)) > \
     RECOMP_SIGNED(b, RECOMP_FLAG_WIDTH(a, b)))
#define TEST_Z(a, b) (((uint32_t)(a) & (uint32_t)(b)) == 0u)
#define TEST_NZ(a, b) (((uint32_t)(a) & (uint32_t)(b)) != 0u)
#define TEST_S(a, b) \
    (RECOMP_SIGNED((uint32_t)(a) & (uint32_t)(b), \
                   RECOMP_FLAG_WIDTH(a, b)) < 0)

/* FSAVE's protected-mode image. Only state represented by the double-stack
 * model is restored: CW, TOP, condition codes and eight registers.
 * ponytail: tags, exception bits and instruction/data pointers are not modeled;
 * add explicit x87 environment tracking before relying on those image fields. */
static inline void recomp_fnsave(uint32_t address, int short_env) {
    unsigned stride = short_env ? 2u : 4u;
    unsigned env = 7u * stride;
    unsigned i;
    for (i = 0; i < env; ++i) MEM8(address + i) = 0;
    MEM16(address) = g_fp_control_word;
    MEM16(address + stride) = (uint16_t)((g_fp_top << 11) | g_fp_cc);
    for (i = 0; i < 8; ++i) {
        uint64_t mant;
        uint16_t se = recomp_f80_store(g_fp_stack[(g_fp_top + i) & 7u], &mant);
        uint32_t slot = address + env + 10u * i;
        MEM32(slot) = (uint32_t)mant;
        MEM32(slot + 4) = (uint32_t)(mant >> 32);
        MEM16(slot + 8) = se;
    }
    g_fp_control_word = 0x037fu;
    g_fp_top = 0;
    g_fp_cc = 0;
    g_fp_cmp = 0;
}

static inline void recomp_frstor(uint32_t address, int short_env) {
    unsigned stride = short_env ? 2u : 4u;
    unsigned env = 7u * stride;
    unsigned i;
    uint16_t status = MEM16(address + stride);
    g_fp_control_word = MEM16(address);
    g_fp_top = (status >> 11) & 7u;
    g_fp_cc = status & 0x4700u;
    g_fp_cmp = (g_fp_cc & 0x0400u) ? 2 : (g_fp_cc & 0x4000u) ? 0
        : (g_fp_cc & 0x0100u) ? -1 : 1;
    for (i = 0; i < 8; ++i) {
        uint32_t slot = address + env + 10u * i;
        uint64_t mant = ((uint64_t)MEM32(slot + 4) << 32) | MEM32(slot);
        g_fp_stack[(g_fp_top + i) & 7u] = recomp_f80_load(mant, MEM16(slot + 8));
    }
}

static inline uint32_t ROL32(uint32_t value, unsigned int count)
{
    count &= 31u;
    return count == 0u ? value : (value << count) | (value >> (32u - count));
}

static inline uint32_t ROR32(uint32_t value, unsigned int count)
{
    count &= 31u;
    return count == 0u ? value : (value >> count) | (value << (32u - count));
}

static inline uint8_t ROL8(uint8_t value, int count)
{
    count = (count & 31) % 8;
    return count ? (uint8_t)((value << count) | (value >> (8 - count))) : value;
}

static inline uint8_t ROR8(uint8_t value, int count)
{
    count = (count & 31) % 8;
    return count ? (uint8_t)((value >> count) | (value << (8 - count))) : value;
}

static inline uint16_t ROR16(uint16_t value, int count)
{
    count = (count & 31) % 16;
    return count ? (uint16_t)((value >> count) | (value << (16 - count))) : value;
}

/* RCL/RCR: rotate through carry over width+1 bits; cf carries in and out. */
static inline uint32_t RC_ROT(uint32_t value, unsigned count, int *cf,
                              unsigned width, int left)
{
    unsigned mod = width + 1u;
    uint64_t mask = width >= 32u ? 0xffffffffull : (((uint64_t)1 << width) - 1u);
    uint64_t x = ((uint64_t)(*cf & 1) << width) | ((uint64_t)value & mask);

    count &= 31u;
    if (width < 32u) {
        count %= mod;
    }
    if (count != 0u) {
        x = left ? (x << count) | (x >> (mod - count))
                 : (x >> count) | (x << (mod - count));
        x &= ((uint64_t)1 << mod) - 1u;
    }
    *cf = (int)((x >> width) & 1u);
    return (uint32_t)(x & mask);
}

#define PUSH32(sp, value) do { \
    uint32_t push_value = (uint32_t)(value); \
    (sp) -= 4u; \
    MEM32(sp) = push_value; \
} while (0)
#define POP32(sp, value) do { \
    (value) = MEM32(sp); \
    (sp) += 4u; \
} while (0)
#define RECOMP_ICALL_SAFE(address, saved_esp) do { \
    recomp_dispatch_indirect_site( \
        (uint32_t)(address), (uint32_t)(saved_esp), __FILE__, __LINE__); \
} while (0)
#define RECOMP_ITAIL(address) do { \
    recomp_dispatch_indirect_site( \
        (uint32_t)(address), g_esp, __FILE__, __LINE__); \
} while (0)

/* ── SSE ────────────────────────────────────────────────────────────────
   RecompXmm and the register file live in runtime.h beside the other
   architectural state. These names alias it so generated code can keep
   writing xmm0..xmm7 directly. */
#define xmm0 recomp_runtime.xmm[0]
#define xmm1 recomp_runtime.xmm[1]
#define xmm2 recomp_runtime.xmm[2]
#define xmm3 recomp_runtime.xmm[3]
#define xmm4 recomp_runtime.xmm[4]
#define xmm5 recomp_runtime.xmm[5]
#define xmm6 recomp_runtime.xmm[6]
#define xmm7 recomp_runtime.xmm[7]

/* Packed transfers keep the guest side under the normal address translation,
   so bounds checks, the cached alias, and the APU aperture still apply. The
   register itself is a host local and is addressed as a host pointer. */
#define XMM_MEM(address) recomp_xmm_mem((uint32_t)(address))
#define XMM_STORE(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg), 16u)
#define XMM_LOAD_LOW(reg, address) \
    recomp_guest_load(&(reg).f[0], (uint32_t)(address), 8u)
#define XMM_STORE_LOW(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg).f[0], 8u)
#define XMM_LOAD_HIGH(reg, address) \
    recomp_guest_load(&(reg).f[2], (uint32_t)(address), 8u)
#define XMM_STORE_HIGH(address, reg) \
    recomp_guest_store((uint32_t)(address), &(reg).f[2], 8u)

static inline RecompXmm recomp_xmm_mem(uint32_t address)
{
    RecompXmm value;

    recomp_guest_load(&value, address, 16u);
    return value;
}

static inline RecompXmm XMM_ZERO(void)
{
    RecompXmm value;

    value.u[0] = 0u;
    value.u[1] = 0u;
    value.u[2] = 0u;
    value.u[3] = 0u;
    return value;
}

/* MOVSS from memory zeroes bits 127:32; MOVSS between registers does not. */
static inline RecompXmm XMM_SCALAR(float lane0)
{
    RecompXmm value = XMM_ZERO();

    value.f[0] = lane0;
    return value;
}

static inline RecompXmm XMM_SCALAR_BITS(uint32_t lane0)
{
    RecompXmm value = XMM_ZERO();

    value.u[0] = lane0;
    return value;
}

static inline RecompXmm XMM_SCALAR_DOUBLE(double lane0)
{
    RecompXmm value = XMM_ZERO();

    value.d[0] = lane0;
    return value;
}

#define RECOMP_XMM_LANEWISE(name, expression) \
    static inline RecompXmm name(RecompXmm a, RecompXmm b) \
    { \
        RecompXmm r; \
        int i; \
        for (i = 0; i < 4; ++i) { \
            expression; \
        } \
        return r; \
    }

RECOMP_XMM_LANEWISE(XMM_ADD, r.f[i] = a.f[i] + b.f[i])
RECOMP_XMM_LANEWISE(XMM_SUB, r.f[i] = a.f[i] - b.f[i])
RECOMP_XMM_LANEWISE(XMM_MUL, r.f[i] = a.f[i] * b.f[i])
RECOMP_XMM_LANEWISE(XMM_DIV, r.f[i] = a.f[i] / b.f[i])
/* MINPS/MAXPS return the second operand when either input is NaN. */
RECOMP_XMM_LANEWISE(XMM_MIN, r.f[i] = a.f[i] < b.f[i] ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_MAX, r.f[i] = a.f[i] > b.f[i] ? a.f[i] : b.f[i])
RECOMP_XMM_LANEWISE(XMM_AND, r.u[i] = a.u[i] & b.u[i])
RECOMP_XMM_LANEWISE(XMM_ANDN, r.u[i] = ~a.u[i] & b.u[i])
RECOMP_XMM_LANEWISE(XMM_OR, r.u[i] = a.u[i] | b.u[i])
RECOMP_XMM_LANEWISE(XMM_XOR, r.u[i] = a.u[i] ^ b.u[i])
RECOMP_XMM_LANEWISE(XMM_CMP_EQ, r.u[i] = a.f[i] == b.f[i] ? 0xffffffffu : 0u)
RECOMP_XMM_LANEWISE(XMM_CMP_LT, r.u[i] = a.f[i] < b.f[i] ? 0xffffffffu : 0u)
RECOMP_XMM_LANEWISE(XMM_CMP_LE, r.u[i] = a.f[i] <= b.f[i] ? 0xffffffffu : 0u)
RECOMP_XMM_LANEWISE(XMM_CMP_NEQ, r.u[i] = a.f[i] != b.f[i] ? 0xffffffffu : 0u)

/* CMPPS/CMPSS immediate: 0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT, 6 NLE,
   7 ORD. The negated forms are true when either side is NaN. */
static inline int recomp_cmp_pred(float a, float b, int predicate)
{
    switch (predicate & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return !(a != a || b != b);
    }
}

static inline RecompXmm XMM_CMP_PRED(RecompXmm a, RecompXmm b, int predicate)
{
    RecompXmm r;

    for (int i = 0; i < 4; ++i) {
        r.u[i] = recomp_cmp_pred(a.f[i], b.f[i], predicate) ? 0xffffffffu : 0u;
    }
    return r;
}

/* SHUFPS takes the low half from the destination and the high half from the
   source. It is the broadcast in every matrix concatenation. */
#if RECOMP_HOST_X86
static inline RecompXmm recomp_xmm_from_native(__m128 value)
{
    RecompXmm r;

    _mm_storeu_ps(r.f, value);
    return r;
}

/* SHUFPS carries an immediate byte; retain it for the native instruction. */
#define XMM_SHUFFLE(a, b, imm) \
    recomp_xmm_from_native(_mm_shuffle_ps( \
        _mm_loadu_ps((a).f), _mm_loadu_ps((b).f), (imm)))
#else
static inline RecompXmm recomp_xmm_shuffle(RecompXmm a, RecompXmm b, unsigned imm)
{
    RecompXmm r;

    r.u[0] = a.u[imm & 3u];
    r.u[1] = a.u[(imm >> 2) & 3u];
    r.u[2] = b.u[(imm >> 4) & 3u];
    r.u[3] = b.u[(imm >> 6) & 3u];
    return r;
}

#define XMM_SHUFFLE(a, b, imm) recomp_xmm_shuffle((a), (b), (unsigned)(imm))
#endif

static inline RecompXmm XMM_UNPACK_LOW(RecompXmm a, RecompXmm b)
{
    RecompXmm r;

    r.u[0] = a.u[0];
    r.u[1] = b.u[0];
    r.u[2] = a.u[1];
    r.u[3] = b.u[1];
    return r;
}

static inline RecompXmm XMM_UNPACK_HIGH(RecompXmm a, RecompXmm b)
{
    RecompXmm r;

    r.u[0] = a.u[2];
    r.u[1] = b.u[2];
    r.u[2] = a.u[3];
    r.u[3] = b.u[3];
    return r;
}

/* MOVLHPS: destination high half takes the source low half. */
static inline RecompXmm XMM_MOVE_LOW_TO_HIGH(RecompXmm a, RecompXmm b)
{
    RecompXmm r = a;

    r.u[2] = b.u[0];
    r.u[3] = b.u[1];
    return r;
}

/* MOVHLPS: destination low half takes the source high half. */
static inline RecompXmm XMM_MOVE_HIGH_TO_LOW(RecompXmm a, RecompXmm b)
{
    RecompXmm r = a;

    r.u[0] = b.u[2];
    r.u[1] = b.u[3];
    return r;
}

static inline uint32_t XMM_MOVEMASK(RecompXmm a)
{
    return ((a.u[0] >> 31) & 1u) | ((a.u[1] >> 30) & 2u) |
           ((a.u[2] >> 29) & 4u) | ((a.u[3] >> 28) & 8u);
}

#if RECOMP_HOST_X86
/* RCPSS/RSQRTSS are table approximations; the host instruction gives the
   guest's bits on Intel, where exact division would not. */
static inline float recomp_rcpss(float x)
{
    return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
}

static inline float recomp_rsqrtss(float x)
{
    return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(x)));
}

/* CVTSS2SI rounds under MXCSR (host default: nearest); CVTTSS2SI chops.
   Out of range and NaN give the integer indefinite 0x80000000. */
static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
    return truncate ? _mm_cvttss_si32(_mm_set_ss(v))
                    : _mm_cvtss_si32(_mm_set_ss(v));
}
#else
/* ponytail: non-x86 hosts have no RCPSS/RSQRTSS table; the exact quotient
   is within Intel's documented 1.5 * 2^-12 error but not bit-identical. */
static inline float recomp_rcpss(float x)
{
    return 1.0f / x;
}

static inline float recomp_rsqrtss(float x)
{
    return 1.0f / sqrtf(x);
}

static inline int32_t MMX_CVT_F2I(float v, int truncate)
{
    /* The default MXCSR rounds to nearest even, as lrintf does under the
       host default rounding mode. */
    if (!(v >= -2147483648.0f && v < 2147483648.0f)) return INT32_MIN;
    return truncate ? (int32_t)v : (int32_t)lrintf(v);
}
#endif

/* ── MMX ────────────────────────────────────────────────────────────────
   The register file lives in runtime.h. Use SSE2's low 64 bits so packed
   operations stay packed without putting the host FPU into MMX mode. */
#define mm0 recomp_runtime.mmx[0]
#define mm1 recomp_runtime.mmx[1]
#define mm2 recomp_runtime.mmx[2]
#define mm3 recomp_runtime.mmx[3]
#define mm4 recomp_runtime.mmx[4]
#define mm5 recomp_runtime.mmx[5]
#define mm6 recomp_runtime.mmx[6]
#define mm7 recomp_runtime.mmx[7]

static inline RecompMmx MMX_MEM(uint32_t address)
{
    RecompMmx value;

    value.q = *recomp_memory_u64(address);
    return value;
}

#define MMX_STORE(address, reg) \
    (*recomp_memory_u64((uint32_t)(address)) = (reg).q)

#if RECOMP_HOST_X86
static inline __m128i recomp_mmx_vector(RecompMmx value)
{
    return _mm_cvtsi64_si128((int64_t)value.q);
}

static inline RecompMmx recomp_mmx_low(__m128i value)
{
    RecompMmx result;
    result.q = (uint64_t)_mm_cvtsi128_si64(value);
    return result;
}

#define RECOMP_MMX_BINOP(name, intrinsic) \
    static inline RecompMmx name(RecompMmx a, RecompMmx b) \
    { \
        return recomp_mmx_low(intrinsic(recomp_mmx_vector(a), recomp_mmx_vector(b))); \
    }

RECOMP_MMX_BINOP(MMX_PADDW, _mm_add_epi16)
RECOMP_MMX_BINOP(MMX_PSUBW, _mm_sub_epi16)
RECOMP_MMX_BINOP(MMX_PMULLW, _mm_mullo_epi16)
RECOMP_MMX_BINOP(MMX_PMULHW, _mm_mulhi_epi16)
RECOMP_MMX_BINOP(MMX_PAVGB, _mm_avg_epu8)
RECOMP_MMX_BINOP(MMX_PUNPCKLWD, _mm_unpacklo_epi16)
RECOMP_MMX_BINOP(MMX_PADDD, _mm_add_epi32)
RECOMP_MMX_BINOP(MMX_PSUBD, _mm_sub_epi32)
RECOMP_MMX_BINOP(MMX_PADDUSW, _mm_adds_epu16)
RECOMP_MMX_BINOP(MMX_PAVGW, _mm_avg_epu16)
RECOMP_MMX_BINOP(MMX_PMINSW, _mm_min_epi16)
RECOMP_MMX_BINOP(MMX_PCMPEQD, _mm_cmpeq_epi32)
RECOMP_MMX_BINOP(MMX_PMADDWD, _mm_madd_epi16)
RECOMP_MMX_BINOP(MMX_PAND, _mm_and_si128)
RECOMP_MMX_BINOP(MMX_POR, _mm_or_si128)
RECOMP_MMX_BINOP(MMX_PXOR, _mm_xor_si128)
RECOMP_MMX_BINOP(MMX_PUNPCKLBW, _mm_unpacklo_epi8)
RECOMP_MMX_BINOP(MMX_PUNPCKLDQ, _mm_unpacklo_epi32)

/* SSE2 variable shifts take the full unsigned count from the low 64 bits. */
#define RECOMP_MMX_SHIFT(name, intrinsic) \
    static inline RecompMmx name(RecompMmx a, uint64_t count) \
    { \
        return recomp_mmx_low(intrinsic(recomp_mmx_vector(a), \
            _mm_cvtsi64_si128((int64_t)count))); \
    }

RECOMP_MMX_SHIFT(MMX_PSLLW, _mm_sll_epi16)
RECOMP_MMX_SHIFT(MMX_PSRAW, _mm_sra_epi16)
RECOMP_MMX_SHIFT(MMX_PSRAD, _mm_sra_epi32)
RECOMP_MMX_SHIFT(MMX_PSLLD, _mm_sll_epi32)
RECOMP_MMX_SHIFT(MMX_PSRLD, _mm_srl_epi32)
RECOMP_MMX_SHIFT(MMX_PSLLQ, _mm_sll_epi64)
RECOMP_MMX_SHIFT(MMX_PSRLQ, _mm_srl_epi64)
RECOMP_MMX_SHIFT(MMX_PSRLW, _mm_srl_epi16)

static inline RecompMmx MMX_PUNPCKHWD(RecompMmx a, RecompMmx b)
{
    return recomp_mmx_low(_mm_srli_si128(
        _mm_unpacklo_epi16(recomp_mmx_vector(a), recomp_mmx_vector(b)), 8));
}

static inline RecompMmx MMX_PUNPCKHBW(RecompMmx a, RecompMmx b)
{
    return recomp_mmx_low(_mm_srli_si128(
        _mm_unpacklo_epi8(recomp_mmx_vector(a), recomp_mmx_vector(b)), 8));
}

static inline RecompMmx MMX_PUNPCKHDQ(RecompMmx a, RecompMmx b)
{
    return recomp_mmx_low(_mm_srli_si128(
        _mm_unpacklo_epi32(recomp_mmx_vector(a), recomp_mmx_vector(b)), 8));
}

static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b)
{
    return recomp_mmx_low(_mm_packs_epi16(
        _mm_unpacklo_epi64(recomp_mmx_vector(a), recomp_mmx_vector(b)),
        _mm_setzero_si128()));
}

static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b)
{
    return recomp_mmx_low(_mm_packus_epi16(
        _mm_unpacklo_epi64(recomp_mmx_vector(a), recomp_mmx_vector(b)),
        _mm_setzero_si128()));
}

static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b)
{
    return recomp_mmx_low(_mm_packs_epi32(
        _mm_unpacklo_epi64(recomp_mmx_vector(a), recomp_mmx_vector(b)),
        _mm_setzero_si128()));
}

#else
static inline int8_t recomp_saturate_i8(int32_t v)
{
    return (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
}

static inline int16_t recomp_saturate_i16(int32_t v)
{
    return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

static inline uint8_t recomp_saturate_u8(int32_t v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

#define RECOMP_MMX_LANES(name, field, count, expr) \
    static inline RecompMmx name(RecompMmx a, RecompMmx b) \
    { \
        RecompMmx r; \
        for (int i = 0; i < (count); ++i) r.field[i] = (expr); \
        return r; \
    }

RECOMP_MMX_LANES(MMX_PADDW, uw, 4, (uint16_t)(a.uw[i] + b.uw[i]))
RECOMP_MMX_LANES(MMX_PSUBW, uw, 4, (uint16_t)(a.uw[i] - b.uw[i]))
RECOMP_MMX_LANES(MMX_PMULLW, uw, 4, (uint16_t)((int32_t)a.w[i] * b.w[i]))
RECOMP_MMX_LANES(MMX_PMULHW, w, 4, (int16_t)(((int32_t)a.w[i] * b.w[i]) >> 16))
RECOMP_MMX_LANES(MMX_PAVGB, ub, 8, (uint8_t)(((unsigned)a.ub[i] + b.ub[i] + 1u) >> 1))
RECOMP_MMX_LANES(MMX_PADDD, ud, 2, a.ud[i] + b.ud[i])
RECOMP_MMX_LANES(MMX_PSUBD, ud, 2, a.ud[i] - b.ud[i])
RECOMP_MMX_LANES(MMX_PADDUSW, uw, 4,
    (uint16_t)((uint32_t)a.uw[i] + b.uw[i] > 0xffffu ? 0xffffu : a.uw[i] + b.uw[i]))
RECOMP_MMX_LANES(MMX_PAVGW, uw, 4, (uint16_t)(((uint32_t)a.uw[i] + b.uw[i] + 1u) >> 1))
RECOMP_MMX_LANES(MMX_PMINSW, w, 4, a.w[i] < b.w[i] ? a.w[i] : b.w[i])
RECOMP_MMX_LANES(MMX_PCMPEQD, ud, 2, a.ud[i] == b.ud[i] ? 0xffffffffu : 0u)
/* 0x8000 * 0x8000 twice wraps to 0x80000000, as on hardware. */
RECOMP_MMX_LANES(MMX_PMADDWD, ud, 2,
    (uint32_t)((int32_t)a.w[2 * i] * b.w[2 * i]) + (uint32_t)((int32_t)a.w[2 * i + 1] * b.w[2 * i + 1]))

static inline RecompMmx MMX_PAND(RecompMmx a, RecompMmx b)
{
    a.q &= b.q;
    return a;
}

static inline RecompMmx MMX_POR(RecompMmx a, RecompMmx b)
{
    a.q |= b.q;
    return a;
}

static inline RecompMmx MMX_PXOR(RecompMmx a, RecompMmx b)
{
    a.q ^= b.q;
    return a;
}

/* Interleave the low (half = 0) or high (half = 1) lanes of a and b. */
#define RECOMP_MMX_UNPACK(name, field, count, half) \
    static inline RecompMmx name(RecompMmx a, RecompMmx b) \
    { \
        RecompMmx r; \
        for (int i = 0; i < (count) / 2; ++i) { \
            r.field[2 * i] = a.field[i + (half) * (count) / 2]; \
            r.field[2 * i + 1] = b.field[i + (half) * (count) / 2]; \
        } \
        return r; \
    }

RECOMP_MMX_UNPACK(MMX_PUNPCKLBW, ub, 8, 0)
RECOMP_MMX_UNPACK(MMX_PUNPCKLWD, uw, 4, 0)
RECOMP_MMX_UNPACK(MMX_PUNPCKLDQ, ud, 2, 0)
RECOMP_MMX_UNPACK(MMX_PUNPCKHBW, ub, 8, 1)
RECOMP_MMX_UNPACK(MMX_PUNPCKHWD, uw, 4, 1)
RECOMP_MMX_UNPACK(MMX_PUNPCKHDQ, ud, 2, 1)

/* Counts above the lane width clear logical shifts and fill arithmetic ones
   with the sign. */
#define RECOMP_MMX_SHIFT(name, field, count, bits, expr) \
    static inline RecompMmx name(RecompMmx a, uint64_t n) \
    { \
        RecompMmx r; \
        for (int i = 0; i < (count); ++i) r.field[i] = n > (bits) - 1u ? 0u : (expr); \
        return r; \
    }

RECOMP_MMX_SHIFT(MMX_PSLLW, uw, 4, 16u, (uint16_t)(a.uw[i] << n))
RECOMP_MMX_SHIFT(MMX_PSRLW, uw, 4, 16u, (uint16_t)(a.uw[i] >> n))
RECOMP_MMX_SHIFT(MMX_PSLLD, ud, 2, 32u, a.ud[i] << n)
RECOMP_MMX_SHIFT(MMX_PSRLD, ud, 2, 32u, a.ud[i] >> n)

static inline RecompMmx MMX_PSLLQ(RecompMmx a, uint64_t n)
{
    a.q = n > 63u ? 0u : a.q << n;
    return a;
}

static inline RecompMmx MMX_PSRLQ(RecompMmx a, uint64_t n)
{
    a.q = n > 63u ? 0u : a.q >> n;
    return a;
}

static inline RecompMmx MMX_PSRAW(RecompMmx a, uint64_t count)
{
    RecompMmx r;
    unsigned shift = count > 15u ? 15u : (unsigned)count;

    for (int i = 0; i < 4; ++i) r.w[i] = (int16_t)(a.w[i] >> shift);
    return r;
}

static inline RecompMmx MMX_PSRAD(RecompMmx a, uint64_t count)
{
    RecompMmx r;
    unsigned shift = count > 31u ? 31u : (unsigned)count;

    for (int i = 0; i < 2; ++i) r.d[i] = a.d[i] >> shift;
    return r;
}

static inline RecompMmx MMX_PACKSSWB(RecompMmx a, RecompMmx b)
{
    RecompMmx r;

    for (int i = 0; i < 4; ++i) {
        r.b[i] = recomp_saturate_i8(a.w[i]);
        r.b[i + 4] = recomp_saturate_i8(b.w[i]);
    }
    return r;
}

static inline RecompMmx MMX_PACKUSWB(RecompMmx a, RecompMmx b)
{
    RecompMmx r;

    for (int i = 0; i < 4; ++i) {
        r.ub[i] = recomp_saturate_u8(a.w[i]);
        r.ub[i + 4] = recomp_saturate_u8(b.w[i]);
    }
    return r;
}

static inline RecompMmx MMX_PACKSSDW(RecompMmx a, RecompMmx b)
{
    RecompMmx r;

    r.w[0] = recomp_saturate_i16(a.d[0]);
    r.w[1] = recomp_saturate_i16(a.d[1]);
    r.w[2] = recomp_saturate_i16(b.d[0]);
    r.w[3] = recomp_saturate_i16(b.d[1]);
    return r;
}
#endif

static inline RecompMmx MMX_FROM32(uint32_t value)
{
    RecompMmx result;
    result.q = value;
    return result;
}

static inline RecompMmx MMX_FROM_PS(float low, float high, int truncate)
{
    RecompMmx result;
    result.d[0] = MMX_CVT_F2I(low, truncate);
    result.d[1] = MMX_CVT_F2I(high, truncate);
    return result;
}

static inline RecompMmx MMX_PSHUFW(RecompMmx value, uint32_t imm)
{
    RecompMmx result;
    for (unsigned lane = 0u; lane < 4u; ++lane) {
        result.uw[lane] = value.uw[(imm >> (lane * 2u)) & 3u];
    }
    return result;
}

static inline RecompMmx MMX_PINSRW(RecompMmx value, uint32_t word, uint32_t imm)
{
    value.uw[imm & 3u] = (uint16_t)word;
    return value;
}

static inline uint32_t MMX_PMOVMSKB(RecompMmx value)
{
#if RECOMP_HOST_X86
    return (uint32_t)_mm_movemask_epi8(recomp_mmx_vector(value));
#else
    uint32_t mask = 0u;
    for (unsigned i = 0u; i < 8u; ++i) mask |= (uint32_t)(value.ub[i] >> 7) << i;
    return mask;
#endif
}

static inline uint32_t MMX_PEXTRW(RecompMmx a, uint32_t imm)
{
    return a.uw[imm & 3u];
}

#endif
