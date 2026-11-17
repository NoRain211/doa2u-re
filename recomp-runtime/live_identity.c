/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

/* The runtime's SHA-256 (xbe/sha.cpp) as a raw digest. */
void doaxbv_sha256_digest(const uint8_t *bytes, size_t size, uint8_t digest[32]);

static const char identity_file[] = "live-identity.bin";
static const char proof_prefix[] = "doa2u-live";

static bool random_secret(uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES])
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES,
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    arc4random_buf(secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    return true;
#else
    size_t got = 0u;
    while (got < RECOMP_LIVE_IDENTITY_SECRET_BYTES) {
        const ssize_t count = getrandom(secret + got, RECOMP_LIVE_IDENTITY_SECRET_BYTES - got, 0);
        if (count <= 0) return false;
        got += (size_t)count;
    }
    return true;
#endif
}

static bool write_secret(const char *path, const uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES])
{
    char temporary[1024];
    if (snprintf(temporary, sizeof temporary, "%s.tmp", path) < 0) return false;
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) return false;
    const bool written = fwrite(secret, 1, RECOMP_LIVE_IDENTITY_SECRET_BYTES, file) ==
        RECOMP_LIVE_IDENTITY_SECRET_BYTES;
    const bool closed = fclose(file) == 0;
    if (!written || !closed || rename(temporary, path) != 0) {
        remove(temporary);
        return false;
    }
    return true;
}

void recomp_live_identity_from_secret(const uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES],
    RecompLiveIdentity *identity)
{
    uint8_t digest[32], material[sizeof proof_prefix - 1u + RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    memcpy(identity->secret, secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    memcpy(material, proof_prefix, sizeof proof_prefix - 1u);
    memcpy(material + sizeof proof_prefix - 1u, secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    doaxbv_sha256_digest(material, sizeof material, identity->proof);
    /* The XUID is derived from the proof the server checks, not from the
       secret, so a server can reject a LOGON whose XUID does not match it: the
       type in the high 16 bits, then the proof's first six bytes as a
       big-endian 48-bit number. */
    doaxbv_sha256_digest(identity->proof, RECOMP_LIVE_IDENTITY_PROOF_BYTES, digest);
    identity->xuid = (uint64_t)RECOMP_LIVE_XUID_TYPE << 48 |
        (uint64_t)digest[0] << 40 | (uint64_t)digest[1] << 32 | (uint64_t)digest[2] << 24 |
        (uint64_t)digest[3] << 16 | (uint64_t)digest[4] << 8 | digest[5];
}

bool recomp_live_identity_load(const char *storage, RecompLiveIdentity *identity)
{
    if (identity == NULL) return false;
    memset(identity, 0, sizeof *identity);
    char path[1024];
    const char *override = getenv("RECOMP_LIVE_IDENTITY");
    if (override != NULL && override[0] != '\0') {
        if (snprintf(path, sizeof path, "%s", override) < 0) return false;
    } else {
        if (storage == NULL || storage[0] == '\0') return false;
        if (snprintf(path, sizeof path, "%s/%s", storage, identity_file) < 0) return false;
    }
    uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        const size_t got = fread(secret, 1, sizeof secret, file);
        const int extra = fgetc(file);
        fclose(file);
        if (got != sizeof secret || extra != EOF) return false; /* a damaged file is not replaced */
    } else if (!random_secret(secret) || !write_secret(path, secret)) {
        return false;
    }
    recomp_live_identity_from_secret(secret, identity);
    return identity->xuid != 0u;
}
