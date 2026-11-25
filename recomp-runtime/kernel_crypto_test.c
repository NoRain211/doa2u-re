#include "kernel_abi.h"

#include <stdio.h>
#include <string.h>

/* Expectations are the published FIPS 180-1 / RFC 3174 SHA-1 vectors plus the
   well-known digest of the empty string. They were not produced by the code
   under test, so a self-consistent but wrong implementation cannot pass. */

enum {
    TEST_MEMORY_BASE = 0x2a000000u,
    TEST_MEMORY_SIZE = 0x00001000u,
    TEST_ENTRY_ESP = TEST_MEMORY_BASE + 0x100u,
    TEST_CONTEXT = TEST_MEMORY_BASE + 0x200u,
    TEST_DIGEST = TEST_MEMORY_BASE + 0x300u,
    TEST_INPUT = TEST_MEMORY_BASE + 0x400u,
    TEST_KEY = TEST_MEMORY_BASE + TEST_MEMORY_SIZE - 64u,
};

typedef struct ShaVector {
    const char *message;
    unsigned repeat;
    const char *digest;
} ShaVector;

static const ShaVector vectors[] = {
    {"", 1u, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
    {"abc", 1u, "a9993e364706816aba3e25717850c26c9cd0d89d"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1u,
     "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    /* One million 'a' is the classic long vector; 200 x 5000 keeps the host
       buffer small while still crossing thousands of block boundaries. */
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 15625u,
     "34aa973cd4c4daa4f61eeb2bdbad27316534016f"},
};

static int expect_digest(
    const char *label, const uint8_t *actual, const char *expected)
{
    char text[2u * RECOMP_SHA_DIGEST_BYTES + 1u];

    for (unsigned i = 0u; i < RECOMP_SHA_DIGEST_BYTES; ++i) {
        snprintf(text + 2u * i, 3u, "%02x", actual[i]);
    }
    if (strcmp(text, expected) == 0) {
        return 1;
    }
    fprintf(
        stderr, "kernel crypto: %s was %s, expected %s\n",
        label, text, expected);
    return 0;
}
static int expect_u32(const char *field, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        return 1;
    }
    fprintf(
        stderr, "kernel crypto: %s was 0x%08x, expected 0x%08x\n",
        field, actual, expected);
    return 0;
}

/* The model alone, with no guest memory involved. */
static int check_model(const ShaVector *vector)
{
    RecompShaContext context;
    uint8_t digest[RECOMP_SHA_DIGEST_BYTES];
    size_t length = strlen(vector->message);

    recomp_kernel_sha_init(&context);
    for (unsigned i = 0u; i < vector->repeat; ++i) {
        recomp_kernel_sha_update(
            &context, (const uint8_t *)vector->message, (uint32_t)length);
    }
    recomp_kernel_sha_final(&context, digest);
    return expect_digest("model digest", digest, vector->digest);
}

/* Feeding one byte at a time must give the same answer as one bulk update:
   that is what proves the buffered-tail path in update is right. */
static int check_byte_at_a_time(const ShaVector *vector)
{
    RecompShaContext context;
    uint8_t digest[RECOMP_SHA_DIGEST_BYTES];
    size_t length = strlen(vector->message);

    if (vector->repeat != 1u) {
        return 1;
    }
    recomp_kernel_sha_init(&context);
    for (size_t i = 0u; i < length; ++i) {
        recomp_kernel_sha_update(
            &context, (const uint8_t *)vector->message + i, 1u);
    }
    recomp_kernel_sha_final(&context, digest);
    return expect_digest("byte-at-a-time digest", digest, vector->digest);
}

static int check_hmac_model(void)
{
    uint8_t key[80], digest[RECOMP_SHA_DIGEST_BYTES];
    const uint8_t message[] = "block boundary";
    int passed = 1;

    /* RFC 2202 cases 1 and 2, with the first message split across inputs. */
    memset(key, 0x0b, 20u);
    recomp_kernel_hmac(key, 20u, (const uint8_t *)"Hi ", 3u,
        (const uint8_t *)"There", 5u, digest);
    passed &= expect_digest("HMAC RFC 2202 case 1", digest,
        "b617318655057264e28bc0b6fb378c8ef146be00");
    recomp_kernel_hmac((const uint8_t *)"Jefe", 4u, NULL, 0u,
        (const uint8_t *)"what do ya want for nothing?", 28u, digest);
    passed &= expect_digest("HMAC RFC 2202 case 2", digest,
        "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
    recomp_kernel_hmac(NULL, 0u, NULL, 0u, NULL, 0u, digest);
    passed &= expect_digest("HMAC empty key and inputs", digest,
        "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d");

    /* Independent Python hmac-SHA1 oracle over bytes(range(64)), not the
       standard HMAC treatment of an oversized key. */
    for (unsigned i = 0u; i < sizeof key; ++i) key[i] = (uint8_t)i;
    recomp_kernel_hmac(key, 64u, message, sizeof message - 1u, NULL, 0u, digest);
    passed &= expect_digest("HMAC 64-byte key", digest,
        "7a2106f58bf458459dd09070dfdd27c0afd2d4f6");
    recomp_kernel_hmac(key, sizeof key, message, sizeof message - 1u, NULL, 0u, digest);
    passed &= expect_digest("HMAC truncates oversized key", digest,
        "7a2106f58bf458459dd09070dfdd27c0afd2d4f6");
    return passed;
}

/* RFC 6229, key 01 02 03 04 05, offsets 0/256; BSD terms are in NOTICE. */
static const uint8_t rc4_key[] = {1u, 2u, 3u, 4u, 5u};
static const uint8_t rc4_start[] = {
    0xb2, 0x39, 0x63, 0x05, 0xf0, 0x3d, 0xc0, 0x27,
    0xcc, 0xc3, 0x52, 0x4a, 0x0a, 0x11, 0x18, 0xa8,
    0x69, 0x82, 0x94, 0x4f, 0x18, 0xfc, 0x82, 0xd5,
    0x89, 0xc4, 0x03, 0xa4, 0x7a, 0x0d, 0x09, 0x19,
};
static const uint8_t rc4_offset_256[] = {
    0x1c, 0xfc, 0xf6, 0x2b, 0x03, 0xed, 0xdb, 0x64,
    0x1d, 0x77, 0xdf, 0xcf, 0x7f, 0x8d, 0x8c, 0x93,
};

static int expect_bytes(const char *label, const void *actual, const void *expected, size_t size)
{
    if (memcmp(actual, expected, size) == 0) return 1;
    fprintf(stderr, "kernel crypto: %s differs\n", label);
    return 0;
}

static int check_rc4_model(void)
{
    RecompRc4Context context, split_context, saved;
    uint8_t bulk[272] = {0}, split[272] = {0}, empty[272] = {0};
    int passed = expect_u32("RC4 context size", sizeof context, RECOMP_RC4_CONTEXT_BYTES);
    recomp_kernel_rc4_key(&context, rc4_key, sizeof rc4_key);
    saved = context;
    recomp_kernel_rc4_key(&context, NULL, 0u);
    passed &= expect_bytes("empty RC4 key preserves context", &context, &saved, sizeof context);
    recomp_kernel_rc4_crypt(&context, NULL, 0u);
    passed &= expect_bytes("empty RC4 input preserves context", &context, &saved, sizeof context);
    recomp_kernel_rc4_crypt(&context, bulk, sizeof bulk);
    passed &= expect_bytes("RC4 RFC 6229 offset 0", bulk, rc4_start, sizeof rc4_start);
    passed &= expect_bytes("RC4 RFC 6229 offset 256", bulk + 256u, rc4_offset_256, sizeof rc4_offset_256);

    recomp_kernel_rc4_key(&split_context, rc4_key, sizeof rc4_key);
    recomp_kernel_rc4_crypt(&split_context, split, 7u);
    recomp_kernel_rc4_crypt(&split_context, split + 7u, sizeof split - 7u);
    passed &= expect_bytes("RC4 split stream", split, bulk, sizeof bulk);
    passed &= expect_bytes("RC4 split state", &split_context, &context, sizeof context);
    recomp_kernel_rc4_key(&context, rc4_key, sizeof rc4_key);
    recomp_kernel_rc4_crypt(&context, bulk, sizeof bulk);
    passed &= expect_bytes("RC4 decrypts in place", bulk, empty, sizeof bulk);
    return passed;
}

static int rc4_bridge_call(RecompFunction function, uint32_t *stack,
    uint32_t context, uint32_t length, uint32_t input)
{
    stack[0] = 0x0010abcdu;
    stack[1] = context;
    stack[2] = length;
    stack[3] = input;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    function();
    return expect_u32("RC4 ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 16u) &
        expect_u32("RC4 return slot", stack[0], 0x0010abcdu);
}

int recomp_kernel_crypto_test(void);

int recomp_kernel_crypto_test(void)
{
    static uint8_t memory[TEST_MEMORY_SIZE];
    const RecompMemoryRegion region = {
        .address = TEST_MEMORY_BASE,
        .size = sizeof memory,
        .data = memory,
    };
    RecompFunction init = recomp_kernel_crypto(335u);
    RecompFunction update = recomp_kernel_crypto(336u);
    RecompFunction final = recomp_kernel_crypto(337u);
    RecompFunction hmac = recomp_kernel_crypto(340u);
    RecompFunction rc4_init = recomp_kernel_crypto(338u);
    RecompFunction rc4_crypt = recomp_kernel_crypto(339u);
    uint32_t *stack;
    uint8_t digest[RECOMP_SHA_DIGEST_BYTES];
    int passed = 1;

    for (unsigned i = 0u; i < sizeof vectors / sizeof vectors[0]; ++i) {
        passed &= check_model(&vectors[i]);
        passed &= check_byte_at_a_time(&vectors[i]);
    }
    passed &= check_hmac_model();
    passed &= check_rc4_model();

    passed &= expect_u32("ordinal 335 is implemented", init != NULL, 1u);
    passed &= expect_u32("ordinal 336 is implemented", update != NULL, 1u);
    passed &= expect_u32("ordinal 337 is implemented", final != NULL, 1u);
    passed &= expect_u32("ordinal 340 is implemented", hmac != NULL, 1u);
    passed &= expect_u32(
        "ordinal 334 stays unimplemented",
        recomp_kernel_crypto(334u) != NULL, 0u);
    passed &= expect_u32("ordinal 338 is implemented", rc4_init != NULL, 1u);
    passed &= expect_u32("ordinal 339 is implemented", rc4_crypt != NULL, 1u);
    if (init == NULL || update == NULL || final == NULL || hmac == NULL ||
        rc4_init == NULL || rc4_crypt == NULL) {
        return 0;
    }

    memset(memory, 0, sizeof memory);
    recomp_runtime_init(&region, 1u, NULL, 0u, NULL, 0u);

    /* Drive "abc" through the three bridges the way the guest does, and check
       both the digest and the stdcall cleanup. */
    for (unsigned i = 0u; i < 3u; ++i) {
        *recomp_memory_i8(TEST_INPUT + i) = (int8_t)"abc"[i];
    }

    stack = (uint32_t *)(memory + (TEST_ENTRY_ESP - TEST_MEMORY_BASE));
    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    *recomp_memory_u32(TEST_CONTEXT) = 0xfeedfaceu;
    init();
    passed &= expect_u32(
        "init ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);
    /* The live state starts after the XDK context's 24-byte unused prefix,
       and that prefix must survive the call untouched. */
    passed &= expect_u32(
        "init seeds H0", *recomp_memory_u32(TEST_CONTEXT + 24u), 0x67452301u);
    passed &= expect_u32(
        "init leaves the prefix alone",
        *recomp_memory_u32(TEST_CONTEXT), 0xfeedfaceu);

    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    stack[2] = TEST_INPUT;
    stack[3] = 3u;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    update();
    passed &= expect_u32(
        "update ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 16u);

    stack[0] = 0x0010abcdu;
    stack[1] = TEST_CONTEXT;
    stack[2] = TEST_DIGEST;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    final();
    passed &= expect_u32(
        "final ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 12u);
    passed &= expect_u32(
        "final return address survives",
        *recomp_memory_u32(TEST_ENTRY_ESP), 0x0010abcdu);
    for (unsigned i = 0u; i < RECOMP_SHA_DIGEST_BYTES; ++i) {
        digest[i] = (uint8_t)*recomp_memory_i8(TEST_DIGEST + i);
    }
    passed &= expect_digest(
        "bridge digest", digest, "a9993e364706816aba3e25717850c26c9cd0d89d");

    /* A null context is the guest's business, not a crash, and must still
       pop its arguments. */
    stack[0] = 0x0010abcdu;
    stack[1] = 0u;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    init();
    passed &= expect_u32(
        "null context ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 8u);

    /* Only the first 64 key bytes exist in guest memory, although the guest
       supplies an 80-byte key length. The bridge must honor truncation. */
    for (unsigned i = 0u; i < 64u; ++i) *recomp_memory_i8(TEST_KEY + i) = (int8_t)i;
    memcpy(recomp_memory(TEST_INPUT, 14u), "block boundary", 14u);
    *recomp_memory_i8(TEST_DIGEST - 1u) = 0x12;
    *recomp_memory_i8(TEST_DIGEST + RECOMP_SHA_DIGEST_BYTES) = 0x34;
    stack[0] = 0x0010abcdu;
    stack[1] = TEST_KEY;
    stack[2] = 80u;
    stack[3] = TEST_INPUT;
    stack[4] = 5u;
    stack[5] = TEST_INPUT + 5u;
    stack[6] = 9u;
    stack[7] = TEST_DIGEST;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    hmac();
    passed &= expect_u32("HMAC ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 32u);
    passed &= expect_u32("HMAC return slot", stack[0], 0x0010abcdu);
    passed &= expect_digest("HMAC bridge split inputs",
        recomp_memory(TEST_DIGEST, RECOMP_SHA_DIGEST_BYTES),
        "7a2106f58bf458459dd09070dfdd27c0afd2d4f6");
    passed &= expect_u32("HMAC leading guard", (uint8_t)*recomp_memory_i8(TEST_DIGEST - 1u), 0x12u);
    passed &= expect_u32("HMAC trailing guard",
        (uint8_t)*recomp_memory_i8(TEST_DIGEST + RECOMP_SHA_DIGEST_BYTES), 0x34u);

    memset(stack + 1u, 0, 6u * sizeof *stack);
    stack[7] = TEST_DIGEST;
    recomp_runtime.registers.esp = TEST_ENTRY_ESP;
    hmac();
    passed &= expect_digest("HMAC bridge null zero spans",
        recomp_memory(TEST_DIGEST, RECOMP_SHA_DIGEST_BYTES),
        "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d");
    passed &= expect_u32("empty HMAC ESP", recomp_runtime.registers.esp, TEST_ENTRY_ESP + 32u);

    memcpy(recomp_memory(TEST_KEY, sizeof rc4_key), rc4_key, sizeof rc4_key);
    memset(recomp_memory(TEST_INPUT, sizeof rc4_start), 0, sizeof rc4_start);
    *recomp_memory_i8(TEST_CONTEXT - 1u) = 0x12;
    *recomp_memory_i8(TEST_CONTEXT + RECOMP_RC4_CONTEXT_BYTES) = 0x34;
    passed &= rc4_bridge_call(rc4_init, stack, TEST_CONTEXT, sizeof rc4_key, TEST_KEY);
    passed &= expect_u32("RC4 initial byte i", (uint8_t)*recomp_memory_i8(TEST_CONTEXT + 256u), 0u);
    passed &= expect_u32("RC4 initial byte j", (uint8_t)*recomp_memory_i8(TEST_CONTEXT + 257u), 0u);
    passed &= rc4_bridge_call(rc4_crypt, stack, TEST_CONTEXT, 7u, TEST_INPUT);
    passed &= rc4_bridge_call(rc4_crypt, stack, TEST_CONTEXT, 9u, TEST_INPUT + 7u);
    passed &= expect_u32("RC4 live byte i", (uint8_t)*recomp_memory_i8(TEST_CONTEXT + 256u), 16u);

    /* Moving opaque guest state must preserve the stream without a host cache. */
    memcpy(recomp_memory(TEST_CONTEXT + 0x600u, RECOMP_RC4_CONTEXT_BYTES),
        recomp_memory(TEST_CONTEXT, RECOMP_RC4_CONTEXT_BYTES), RECOMP_RC4_CONTEXT_BYTES);
    passed &= rc4_bridge_call(rc4_crypt, stack, TEST_CONTEXT + 0x600u, 16u, TEST_INPUT + 16u);
    passed &= expect_bytes("RC4 bridge split/moved stream",
        recomp_memory(TEST_INPUT, sizeof rc4_start), rc4_start, sizeof rc4_start);
    passed &= expect_u32("RC4 leading context guard", (uint8_t)*recomp_memory_i8(TEST_CONTEXT - 1u), 0x12u);
    passed &= expect_u32("RC4 exact 258-byte end guard",
        (uint8_t)*recomp_memory_i8(TEST_CONTEXT + RECOMP_RC4_CONTEXT_BYTES), 0x34u);
    uint8_t saved_rc4[RECOMP_RC4_CONTEXT_BYTES];
    memcpy(saved_rc4, recomp_memory(TEST_CONTEXT, sizeof saved_rc4), sizeof saved_rc4);
    passed &= rc4_bridge_call(rc4_init, stack, TEST_CONTEXT, 0u, 0u);
    passed &= rc4_bridge_call(rc4_crypt, stack, TEST_CONTEXT, 0u, 0u);
    passed &= expect_bytes("RC4 empty bridge spans preserve state",
        recomp_memory(TEST_CONTEXT, sizeof saved_rc4), saved_rc4, sizeof saved_rc4);

    return passed;
}
