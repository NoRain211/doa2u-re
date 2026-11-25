#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"

/* An XMM register is 128 bits. The lifter used to declare it as a scalar host
   float, so every packed move transferred 4 of its 16 bytes and every packed
   ALU op was dropped to a comment. These check the replacement semantics that
   the generated code now depends on. */

static int expect_float(const char *check, float actual, float expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "sse: %s was %f, expected %f\n",
        check,
        (double)actual,
        (double)expected);
    return 0;
}
static int expect_u32(const char *check, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr,
        "sse: %s was 0x%08lx, expected 0x%08lx\n",
        check,
        (unsigned long)actual,
        (unsigned long)expected);
    return 0;
}

/* Bit views of a float; recomp_types.h redirects memcpy to guest memory. */
static uint32_t float_bits(float value)
{
    RecompXmm lanes;
    lanes.f[0] = value;
    return lanes.u[0];
}

static float bits_float(uint32_t bits)
{
    RecompXmm lanes;
    lanes.u[0] = bits;
    return lanes.f[0];
}

static RecompXmm row(float a, float b, float c, float d)
{
    RecompXmm value;

    value.f[0] = a;
    value.f[1] = b;
    value.f[2] = c;
    value.f[3] = d;
    return value;
}


/* Compare every packed helper with scalar lanes, including saturation and
   full 64-bit shift counts. The runtime uses only the low half of SSE2. */
static int mmx_native_lanes(void)
{
    const uint64_t counts[] = {0, 1, 13, 15, 16, 31, 32, 63, 64, UINT64_MAX,
                              UINT64_C(0x100000000)};
    uint32_t seed = 1u;
    for (unsigned n = 0; n < 4096; ++n) {
        RecompMmx a, b, expected[32] = {0};
        for (unsigned i = 0; i < 4; ++i) {
            seed = seed * 1664525u + 1013904223u;
            a.uw[i] = (uint16_t)(seed >> 16);
            seed = seed * 1664525u + 1013904223u;
            b.uw[i] = (uint16_t)(seed >> 16);
        }
        uint64_t count = counts[n % (sizeof counts / sizeof counts[0])];
        RecompMmx actual[] = {MMX_PADDW(a,b), MMX_PSUBW(a,b), MMX_PMULLW(a,b),
            MMX_PMULHW(a,b), MMX_PAVGB(a,b), MMX_PSLLW(a,count),
            MMX_PSRAW(a,count), MMX_PSRAD(a,count), MMX_PUNPCKLWD(a,b),
            MMX_PUNPCKHWD(a,b), MMX_PACKUSWB(a,b), MMX_PACKSSDW(a,b),
            MMX_PADDD(a,b), MMX_PSUBD(a,b), MMX_PADDUSW(a,b), MMX_PAVGW(a,b),
            MMX_PMINSW(a,b), MMX_PCMPEQD(a,b), MMX_PMADDWD(a,b), MMX_PAND(a,b),
            MMX_POR(a,b), MMX_PXOR(a,b), MMX_PSLLD(a,count), MMX_PSRLD(a,count),
            MMX_PSLLQ(a,count), MMX_PSRLQ(a,count), MMX_PSRLW(a,count),
            MMX_PUNPCKLBW(a,b), MMX_PUNPCKHBW(a,b), MMX_PUNPCKLDQ(a,b),
            MMX_PUNPCKHDQ(a,b), MMX_PACKSSWB(a,b)};
        _Static_assert(sizeof actual == sizeof expected, "MMX operation count");
        for (unsigned i = 0; i < 4; ++i) {
            int32_t product = (int32_t)a.w[i] * b.w[i];
            expected[0].uw[i] = (uint16_t)(a.uw[i] + b.uw[i]);
            expected[1].uw[i] = (uint16_t)(a.uw[i] - b.uw[i]);
            expected[2].uw[i] = (uint16_t)product;
            expected[3].w[i] = (int16_t)(product >> 16);
            expected[5].uw[i] = count >= 16 ? 0 : (uint16_t)(a.uw[i] << count);
            expected[6].w[i] = (int16_t)(a.w[i] >> (count >= 16 ? 15 : count));
            expected[10].ub[i] = a.w[i] < 0 ? 0 : a.w[i] > 255 ? 255 : (uint8_t)a.w[i];
            expected[10].ub[i+4] = b.w[i] < 0 ? 0 : b.w[i] > 255 ? 255 : (uint8_t)b.w[i];
            uint32_t sum = (uint32_t)a.uw[i] + b.uw[i];
            expected[14].uw[i] = sum > 65535u ? 65535u : (uint16_t)sum;
            expected[15].uw[i] = (uint16_t)((sum + 1u) / 2u);
            expected[16].w[i] = a.w[i] < b.w[i] ? a.w[i] : b.w[i];
            expected[26].uw[i] = count >= 16 ? 0 : (uint16_t)(a.uw[i] >> count);
            expected[27].ub[2*i] = a.ub[i];
            expected[27].ub[2*i+1] = b.ub[i];
            expected[28].ub[2*i] = a.ub[i+4];
            expected[28].ub[2*i+1] = b.ub[i+4];
            expected[31].b[i] = a.w[i] < -128 ? -128 : a.w[i] > 127 ? 127 : (int8_t)a.w[i];
            expected[31].b[i+4] = b.w[i] < -128 ? -128 : b.w[i] > 127 ? 127 : (int8_t)b.w[i];
        }
        for (unsigned i = 0; i < 8; ++i)
            expected[4].ub[i] = (uint8_t)(((unsigned)a.ub[i] + b.ub[i] + 1) / 2);
        for (unsigned i = 0; i < 2; ++i) {
            /* Floor division by 2^count: arithmetic shift without signed >>. */
            const int64_t unit = (int64_t)1 << (count >= 32 ? 31 : count);
            expected[7].d[i] = (int32_t)((a.d[i] - (a.d[i] < 0 ? unit - 1 : 0)) / unit);
            expected[8].uw[2*i] = a.uw[i];
            expected[8].uw[2*i+1] = b.uw[i];
            expected[9].uw[2*i] = a.uw[i+2];
            expected[9].uw[2*i+1] = b.uw[i+2];
            expected[11].w[i] = a.d[i] < -32768 ? -32768 : a.d[i] > 32767 ? 32767 : (int16_t)a.d[i];
            expected[11].w[i+2] = b.d[i] < -32768 ? -32768 : b.d[i] > 32767 ? 32767 : (int16_t)b.d[i];
            expected[12].ud[i] = a.ud[i] + b.ud[i];
            expected[13].ud[i] = a.ud[i] - b.ud[i];
            expected[17].ud[i] = a.ud[i] == b.ud[i] ? UINT32_MAX : 0u;
            expected[18].ud[i] = (uint32_t)((int32_t)a.w[2*i] * b.w[2*i]) +
                (uint32_t)((int32_t)a.w[2*i+1] * b.w[2*i+1]);
            expected[22].ud[i] = count >= 32 ? 0 : a.ud[i] << count;
            expected[23].ud[i] = count >= 32 ? 0 : a.ud[i] >> count;
        }
        expected[19].q = a.q & b.q;
        expected[20].q = a.q | b.q;
        expected[21].q = a.q ^ b.q;
        expected[24].q = count >= 64 ? 0 : a.q << count;
        expected[25].q = count >= 64 ? 0 : a.q >> count;
        expected[29].ud[0] = a.ud[0]; expected[29].ud[1] = b.ud[0];
        expected[30].ud[0] = a.ud[1]; expected[30].ud[1] = b.ud[1];
        uint32_t mask = 0u;
        for (unsigned i = 0; i < 8; ++i) mask |= (uint32_t)(a.ub[i] >> 7) << i;
        if (MMX_PMOVMSKB(a) != mask) return 0;
        for (unsigned i = 0; i < sizeof actual / sizeof actual[0]; ++i) {
            if (actual[i].q != expected[i].q) {
                fprintf(stderr, "mmx: operation %u sample %u count %llu differs\n",
                    i, n, (unsigned long long)count);
                return 0;
            }
        }
    }
    return 1;
}

int recomp_sse_semantics_test(void)
{
    int passed = mmx_native_lanes();
    RecompXmm x = row(1.0f, 2.0f, 3.0f, 4.0f);
    RecompXmm mask;
    RecompXmm r;
    int i;

    /* shufps dst, src, 0 broadcasts lane 0. This is the multiplier in every
       matrix concatenation, and it used to execute as nothing. */
    r = XMM_SHUFFLE(x, x, 0);
    for (i = 0; i < 4; ++i) {
        passed &= expect_float("shufps broadcast", r.f[i], 1.0f);
    }

    /* 0x4E swaps halves, taking the low half from dst and high from src. */
    r = XMM_SHUFFLE(x, x, 0x4E);
    passed &= expect_float("shufps 0x4E lane0", r.f[0], 3.0f);
    passed &= expect_float("shufps 0x4E lane1", r.f[1], 4.0f);
    passed &= expect_float("shufps 0x4E lane2", r.f[2], 1.0f);
    passed &= expect_float("shufps 0x4E lane3", r.f[3], 2.0f);

    /* IDCT rotates lanes before scalar stores; preserve all bits, including
       NaN payloads and signed zero, when lowering to the native shuffle. */
    {
        RecompXmm a = {.u = {0x80000000u, 0x7fc12345u, 0x3f800000u, 0x40000000u}};
        RecompXmm b = {.u = {0xffc54321u, 0x00000000u, 0xbf800000u, 0xc0000000u}};
        r = XMM_SHUFFLE(a, b, 0xE5);
        passed &= expect_u32("shufps E5 lane0", r.u[0], a.u[1]);
        passed &= expect_u32("shufps E5 lane1", r.u[1], a.u[1]);
        passed &= expect_u32("shufps E5 lane2", r.u[2], b.u[2]);
        passed &= expect_u32("shufps E5 lane3", r.u[3], b.u[3]);
        r = XMM_SHUFFLE(a, b, 0xE6);
        passed &= expect_u32("shufps E6 lane0", r.u[0], a.u[2]);
        r = XMM_SHUFFLE(a, b, 0xE7);
        passed &= expect_u32("shufps E7 lane0", r.u[0], a.u[3]);
        r = XMM_SHUFFLE(a, b, 0);
        passed &= expect_u32("shufps signed zero", r.u[0], a.u[0]);
        passed &= expect_u32("shufps source NaN", r.u[2], b.u[0]);
    }

    /* Packed arithmetic must touch all four lanes, not just lane 0. */
    r = XMM_MUL(x, row(2.0f, 2.0f, 2.0f, 2.0f));
    passed &= expect_float("mulps lane0", r.f[0], 2.0f);
    passed &= expect_float("mulps lane3", r.f[3], 8.0f);
    r = XMM_ADD(x, x);
    passed &= expect_float("addps lane2", r.f[2], 6.0f);
    r = XMM_SUB(x, row(1.0f, 1.0f, 1.0f, 1.0f));
    passed &= expect_float("subps lane1", r.f[1], 1.0f);
    r = XMM_DIV(x, row(2.0f, 2.0f, 2.0f, 2.0f));
    passed &= expect_float("divps lane3", r.f[3], 2.0f);

    /* andps with a sign mask is fabs. It is only correct on the integer
       lanes, which is why the union carries both views. */
    mask.u[0] = 0x7fffffffu;
    mask.u[1] = 0x7fffffffu;
    mask.u[2] = 0x7fffffffu;
    mask.u[3] = 0x7fffffffu;
    r = XMM_AND(row(-5.0f, -6.0f, 7.0f, -8.0f), mask);
    passed &= expect_float("andps fabs lane0", r.f[0], 5.0f);
    passed &= expect_float("andps fabs lane1", r.f[1], 6.0f);
    passed &= expect_float("andps fabs lane2", r.f[2], 7.0f);
    passed &= expect_float("andps fabs lane3", r.f[3], 8.0f);

    /* xorps with the sign bit negates. */
    mask.u[0] = 0x80000000u;
    mask.u[1] = 0x80000000u;
    mask.u[2] = 0x80000000u;
    mask.u[3] = 0x80000000u;
    r = XMM_XOR(x, mask);
    passed &= expect_float("xorps negate lane0", r.f[0], -1.0f);
    passed &= expect_float("xorps negate lane3", r.f[3], -4.0f);

    r = XMM_ANDN(mask, x);
    passed &= expect_float("andnps lane0", r.f[0], 1.0f);

    /* Comparison results are lane masks, not booleans. */
    r = XMM_CMP_LT(x, row(2.0f, 2.0f, 2.0f, 2.0f));
    passed &= expect_u32("cmpltps lane0", r.u[0], 0xffffffffu);
    passed &= expect_u32("cmpltps lane1", r.u[1], 0u);

    r = XMM_MIN(x, row(2.0f, 2.0f, 2.0f, 2.0f));
    passed &= expect_float("minps lane3", r.f[3], 2.0f);
    r = XMM_MAX(x, row(2.0f, 2.0f, 2.0f, 2.0f));
    passed &= expect_float("maxps lane0", r.f[0], 2.0f);

    r = XMM_UNPACK_LOW(x, row(5.0f, 6.0f, 7.0f, 8.0f));
    passed &= expect_float("unpcklps lane0", r.f[0], 1.0f);
    passed &= expect_float("unpcklps lane1", r.f[1], 5.0f);
    passed &= expect_float("unpcklps lane2", r.f[2], 2.0f);
    passed &= expect_float("unpcklps lane3", r.f[3], 6.0f);

    r = XMM_UNPACK_HIGH(x, row(5.0f, 6.0f, 7.0f, 8.0f));
    passed &= expect_float("unpckhps lane0", r.f[0], 3.0f);
    passed &= expect_float("unpckhps lane1", r.f[1], 7.0f);

    r = XMM_MOVE_LOW_TO_HIGH(x, row(5.0f, 6.0f, 7.0f, 8.0f));
    passed &= expect_float("movlhps lane0", r.f[0], 1.0f);
    passed &= expect_float("movlhps lane2", r.f[2], 5.0f);
    passed &= expect_float("movlhps lane3", r.f[3], 6.0f);

    r = XMM_MOVE_HIGH_TO_LOW(x, row(5.0f, 6.0f, 7.0f, 8.0f));
    passed &= expect_float("movhlps lane0", r.f[0], 7.0f);
    passed &= expect_float("movhlps lane2", r.f[2], 3.0f);

    /* movmskps used to return a hardcoded 0, and it feeds branches. */
    passed &= expect_u32(
        "movmskps", XMM_MOVEMASK(row(-1.0f, -1.0f, 1.0f, -1.0f)), 0xBu);
    passed &= expect_u32("movmskps positive", XMM_MOVEMASK(x), 0u);

    /* A scalar load zeroes bits 127:32. */
    r = XMM_SCALAR(9.0f);
    passed &= expect_float("movss lane0", r.f[0], 9.0f);
    passed &= expect_u32("movss lane1 zeroed", r.u[1], 0u);
    passed &= expect_u32("movss lane3 zeroed", r.u[3], 0u);

    r = XMM_ZERO();
    passed &= expect_u32("xorps self lane0", r.u[0], 0u);
    passed &= expect_u32("xorps self lane3", r.u[3], 0u);

    {
        /* Dword lanes (low, high): (-32768, 32767) and (-40000, 40000). */
        RecompMmx a = {.q = UINT64_C(0x00007fffffff8000)};
        RecompMmx b = {.q = UINT64_C(0x00009c40ffff63c0)};
        uint64_t packed = MMX_PACKSSDW(a, b).q;
        passed &= expect_u32("packssdw boundaries", (uint32_t)packed, 0x7fff8000u);
        passed &= expect_u32("packssdw saturation", (uint32_t)(packed >> 32), 0x7fff8000u);
        a.q = UINT64_C(0x00000002ffffffff); /* (-1, 2) */
        b.q = UINT64_C(0x00000004fffffffd); /* (-3, 4) */
        packed = MMX_PACKSSDW(a, b).q;
        passed &= expect_u32("packssdw destination order", (uint32_t)packed, 0x0002ffffu);
        passed &= expect_u32("packssdw source order", (uint32_t)(packed >> 32), 0x0004fffdu);
        a.q = UINT64_C(0x1180fe01ffff0000);
        b.q = UINT64_C(0x127f0102fffe0100);
        uint64_t averaged = MMX_PAVGB(a, b).q;
        passed &= expect_u32("pavgb unsigned extremes", (uint32_t)averaged, 0xffff0100u);
        passed &= expect_u32("pavgb rounded lanes", (uint32_t)(averaged >> 32), 0x12808002u);
    }

    {
        RecompMmx value = {.q = UINT64_C(0x4444333322221111)};
        passed &= expect_u32("pshufw reverse", MMX_PSHUFW(value, 0x1bu).q ==
            UINT64_C(0x1111222233334444), 1u);
        passed &= expect_u32("pshufw broadcast", MMX_PSHUFW(value, 0x55u).q ==
            UINT64_C(0x2222222222222222), 1u);
        passed &= expect_u32("pshufw identity", MMX_PSHUFW(value, 0xe4u).q == value.q, 1u);
        passed &= expect_u32("pinsrw masked index and low word",
            MMX_PINSRW(value, 0xdeadbeefu, 7u).q == UINT64_C(0xbeef333322221111), 1u);
        passed &= expect_u32("movd zeroes high half", MMX_FROM32(0xdeadbeefu).q ==
            UINT64_C(0x00000000deadbeef), 1u);
        value.q = UINT64_C(0x8000800080008000);
        passed &= expect_u32("pmaddwd signed overflow", MMX_PMADDWD(value, value).q ==
            UINT64_C(0x8000000080000000), 1u);
        passed &= expect_u32("pcmpeqd matching lanes", MMX_PCMPEQD(value, value).q == UINT64_MAX, 1u);
        RecompMmx left = {.w = {-129, -128, 127, 128}};
        RecompMmx right = {.w = {-32768, 32767, -1, 1}};
        passed &= expect_u32("packsswb signed boundaries", MMX_PACKSSWB(left, right).q ==
            UINT64_C(0x01ff7f807f7f8080), 1u);
        value = MMX_FROM_PS(-1.5f, 2.5f, 0);
        passed &= expect_u32("cvtps2pi signed rounding", value.ud[0], 0xfffffffeu);
        passed &= expect_u32("cvtps2pi ties to even", value.ud[1], 2u);
        value = MMX_FROM_PS(-1.9f, 2.9f, 1);
        passed &= expect_u32("cvttps2pi signed truncation", value.ud[0], UINT32_MAX);
        passed &= expect_u32("cvttps2pi second lane", value.ud[1], 2u);
        value = MMX_FROM_PS(NAN, INFINITY, 0);
        passed &= expect_u32("cvtps2pi integer indefinite", value.q ==
            UINT64_C(0x8000000080000000), 1u);
#if RECOMP_HOST_X86
        /* Portable hosts round to nearest; only x86 follows a changed MXCSR. */
        unsigned control = _mm_getcsr();
        _MM_SET_ROUNDING_MODE(_MM_ROUND_DOWN);
        value = MMX_FROM_PS(-1.2f, 2.9f, 0);
        _mm_setcsr(control);
        passed &= expect_u32("cvtps2pi MXCSR rounding", value.ud[0], 0xfffffffeu);
        passed &= expect_u32("cvtps2pi MXCSR second lane", value.ud[1], 2u);
#endif
    }

    {
        /* Helpers behind the x87 and SSE lifts, at values where the old
           lifts disagreed with the CPU. */
        uint64_t mant = 0;
        uint16_t se = recomp_f80_store(-2.5, &mant);
        passed &= expect_u32("fstp tbyte sign and exponent", se, 0xc000u);
        passed &= expect_u32("fstp tbyte significand", (uint32_t)(mant >> 32), 0xa0000000u);
        passed &= expect_float("fld tbyte 1.0",
            (float)recomp_f80_load(UINT64_C(0x8000000000000000), 0x3fffu), 1.0f);
        /* remquo rounds 3.5 to 4; fprem chops to 3, which C3/C1 report. */
        passed &= expect_float("fprem 7 by 2", (float)recomp_fprem(7.0, 2.0, 0), 1.0f);
        passed &= expect_u32("fprem quotient bits", g_fp_cc, 0x4200u);
        passed &= expect_float("fprem1 5.5 by 2", (float)recomp_fprem(5.5, 2.0, 1), -0.5f);
        passed &= expect_u32("fsin at 2^63 is refused", (uint32_t)recomp_fp_trig_in_range(1e19), 0u);
        passed &= expect_u32("fsin range sets C2", g_fp_cc & 0x0400u, 0x0400u);
        passed &= expect_float("fscale chops its scale", (float)recomp_fscale(1.5, 2.9), 6.0f);
        passed &= expect_u32("cvtss2si ties to even", (uint32_t)MMX_CVT_F2I(2.5f, 0), 2u);
        passed &= expect_u32("cvttss2si chops", (uint32_t)MMX_CVT_F2I(-2.7f, 1), 0xfffffffeu);
        passed &= expect_u32("cvtss2si NaN", (uint32_t)MMX_CVT_F2I(NAN, 0), 0x80000000u);
        /* Estimates with Intel's bits, not 1/x (see recomp_rcpss). */
        passed &= expect_u32("rcpss 3", float_bits(recomp_rcpss(3.0f)), 0x3eaaa000u);
        passed &= expect_u32("rcpss -0", float_bits(recomp_rcpss(-0.0f)), 0xff800000u);
        passed &= expect_u32("rcpss denormal", float_bits(recomp_rcpss(bits_float(0x007fffffu))), 0x7f800000u);
        passed &= expect_u32("rcpss tiny result", float_bits(recomp_rcpss(bits_float(0xfe800000u))), 0x80000000u);
        passed &= expect_u32("rcpss infinity", float_bits(recomp_rcpss(INFINITY)), 0u);
        passed &= expect_u32("rsqrtss 1", float_bits(recomp_rsqrtss(1.0f)), 0x3f7ff000u);
        passed &= expect_u32("rsqrtss 2", float_bits(recomp_rsqrtss(2.0f)), 0x3f34f800u);
        passed &= expect_u32("rsqrtss 0", float_bits(recomp_rsqrtss(0.0f)), 0x7f800000u);
        passed &= expect_u32("rsqrtss negative", float_bits(recomp_rsqrtss(-1.0f)), 0xffc00000u);
        passed &= expect_u32("rsqrtss infinity", float_bits(recomp_rsqrtss(INFINITY)), 0u);
        /* Every table interval: RCPSS over [1, 2), RSQRTSS over [1, 4) for
           both exponent parities, hashed from the 12-bit results. */
        {
            uint32_t rcp = 0u, rsqrt = 0u;
            for (uint32_t i = 0u; i < 2048u; ++i) {
                rcp = rcp * 31u + (float_bits(recomp_rcpss(bits_float(0x3f800000u + (i << 12)))) >> 11);
                rsqrt = rsqrt * 31u + (float_bits(recomp_rsqrtss(bits_float(0x3f800000u + (i << 13)))) >> 11);
            }
            passed &= expect_u32("rcpss table", rcp, 0x935c1ceau);
            passed &= expect_u32("rsqrtss table", rsqrt, 0x11e62ffbu);
        }
    }

    {
        uint8_t image[112];
        RecompMemoryRegion region = {0x10000u, sizeof(image), image};
        recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
        MEM32(0x10000u) = 0xfffffffeu;
        passed &= expect_u32("atomic add returns old value",
            RECOMP_ATOMIC_ADD32(XBOX_PTR(0x10000u), 5u), 0xfffffffeu);
        passed &= expect_u32("atomic add wraps", MEM32(0x10000u), 3u);
        passed &= expect_u32("atomic add negative delta",
            RECOMP_ATOMIC_ADD32(XBOX_PTR(0x10000u), -4), 3u);
        passed &= expect_u32("atomic add negative wrap", MEM32(0x10000u), UINT32_MAX);
        passed &= expect_u32("atomic add zero delta",
            RECOMP_ATOMIC_ADD32(XBOX_PTR(0x10000u), 0u), UINT32_MAX);
        passed &= expect_u32("atomic add zero preserves value", MEM32(0x10000u), UINT32_MAX);
        for (int short_env = 0; short_env < 2; ++short_env) {
            (memset)(image, 0xa5, sizeof(image));
            g_fp_top = 5;
            g_fp_control_word = 0x0a7fu;
            g_fp_cc = 0x4100u;
            for (i = 0; i < 8; ++i) g_fp_stack[i] = i - 2.5;
            recomp_fnsave(0x10000u, short_env);
            passed &= expect_u32("fnsave reset TOP", g_fp_top, 0u);
            passed &= expect_u32("fnsave reset CW", g_fp_control_word, 0x037fu);
            passed &= expect_u32("fnsave reset CC", g_fp_cc, 0u);
            passed &= expect_u32("fnsave image CW", MEM16(0x10000u), 0x0a7fu);
            passed &= expect_u32("fnsave image SW",
                MEM16(0x10000u + (short_env ? 2u : 4u)), 0x6900u);
            passed &= expect_u32("fnsave image end", image[short_env ? 94 : 108], 0xa5u);
            (memset)(g_fp_stack, 0, sizeof(g_fp_stack));
            /* A callee can change the saved environment before FRSTOR. */
            MEM16(0x10000u) = 0x027fu;
            recomp_frstor(0x10000u, short_env);
            passed &= expect_u32("frstor TOP", g_fp_top, 5u);
            passed &= expect_u32("frstor modified CW", g_fp_control_word, 0x027fu);
            passed &= expect_u32("frstor CC", g_fp_cc, 0x4100u);
            for (i = 0; i < 8; ++i)
                passed &= expect_float("frstor register", (float)g_fp_stack[i], i - 2.5f);
        }
        recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    }

    {
        uint8_t physical_zero[0x1000] = {0};
        uint8_t pcr[XBOX_PCR_SIZE] = {0};
        uint8_t before[XBOX_PCR_SIZE];
        const RecompMemoryRegion regions[] = {
            {0u, sizeof physical_zero, physical_zero},
            {XBOX_PCR_BASE, sizeof pcr, pcr},
        };
        recomp_runtime_init(regions, 2u, NULL, 0u, NULL, 0u);
        passed &= expect_u32("generated FS base", XBOX_FS_BASE, XBOX_PCR_BASE);
        MEM32(XBOX_FS_BASE) = 0xffffffffu;
        MEM32(XBOX_FS_BASE + 4u) = XBOX_STARTUP_THREAD_STACK_SLOT + 0xb4u;
        MEM32(XBOX_FS_BASE + 0x20u) = XBOX_PCR_BASE + 0x28u;
        MEM32(XBOX_FS_BASE + 0x28u) = XBOX_STARTUP_THREAD_OBJECT;
        (memcpy)(before, pcr, sizeof before);
        recomp_guest_memset(RECOMP_XBOX_CACHED_ALIAS, 0xcc, sizeof physical_zero);
        MEM32(RECOMP_XBOX_CACHED_ALIAS + 0x278u) = 0x12345678u;
        passed &= expect_u32("cached page zero remains guest memory", MEM32(0u), 0xccccccccu);
        passed &= expect_u32("cached write reaches physical zero", MEM32(0x278u), 0x12345678u);
        passed &= expect_u32("cached page zero preserves PCR", memcmp(before, pcr, sizeof pcr) == 0, 1u);
        passed &= expect_u32("cached page zero preserves TLS", MEM32(XBOX_FS_BASE + 4u), XBOX_STARTUP_THREAD_STACK_SLOT + 0xb4u);
        passed &= expect_u32("cached page zero preserves thread", MEM32(XBOX_FS_BASE + 0x28u), XBOX_STARTUP_THREAD_OBJECT);
        passed &= expect_u32("cached page zero preserves PRCB callback", MEM32(MEM32(XBOX_FS_BASE + 0x20u) + 0x250u), 0u);
        recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    }

    /* Packed transfers: whole guest RAM takes the inline path, a small region the translated one. */
    for (int whole_ram = 0; whole_ram < 2; ++whole_ram) {
        const size_t size = whole_ram ? RECOMP_XBOX_RAM_SIZE : 0x40u;
        uint8_t *image = calloc(1u, size);
        RecompMemoryRegion region = {0u, (uint32_t)size, image};
        RecompXmm value = row(1.0f, 2.0f, 3.0f, 4.0f), low = row(0.0f, 0.0f, 0.0f, 0.0f);
        const float written[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);
        passed &= expect_u32("packed path", recomp_fast_ram != NULL, (uint32_t)whole_ram);
        XMM_STORE(0x10u, value);
        passed &= expect_u32("movaps store", (memcmp)(image + 0x10, written, 16u) == 0, 1u);
        passed &= expect_float("movaps load", XMM_MEM(0x10u).f[3], 4.0f);
        if (whole_ram)
            passed &= expect_float("movaps load through the cached alias",
                XMM_MEM(RECOMP_XBOX_CACHED_ALIAS + 0x10u).f[2], 3.0f);
        XMM_LOAD_LOW(low, 0x18u);
        XMM_LOAD_HIGH(low, 0x10u);
        passed &= expect_float("movlps load", low.f[1], 4.0f);
        passed &= expect_float("movhps load", low.f[2], 1.0f);
        XMM_STORE_HIGH(0x20u, value);
        XMM_STORE_LOW(0x28u, value);
        passed &= expect_float("movhps store", MEMF(0x24u), 4.0f);
        passed &= expect_float("movlps store", MEMF(0x28u), 1.0f);
        recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
        free(image);
    }

    return passed;
}
