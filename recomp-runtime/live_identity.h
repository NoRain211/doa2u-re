/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_IDENTITY_H
#define DOA2U_RECOMP_LIVE_IDENTITY_H

/* The Xbox Live account this install logs on with: a 32-byte
   secret kept beside the netplay settings, the XUID the matchmaker knows it
   by, and the proof it sends with every LOGON. The same secret always gives
   the same account, so friends and stats survive a restart. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_LIVE_IDENTITY_SECRET_BYTES = 32,
    RECOMP_LIVE_IDENTITY_PROOF_BYTES = 32,
    /* The high 16 bits of an Xbox Live XUID; the low 48 come from the secret. */
    RECOMP_LIVE_XUID_TYPE = 0x0009u,
};

typedef struct RecompLiveIdentity {
    uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    uint64_t xuid;
    uint8_t proof[RECOMP_LIVE_IDENTITY_PROOF_BYTES];
} RecompLiveIdentity;

/* Reads <storage>/live-identity.bin, or the file RECOMP_LIVE_IDENTITY names,
   and makes a random secret there on first use. False when the file is
   damaged, cannot be written, or no secret can be made. */
bool recomp_live_identity_load(const char *storage, RecompLiveIdentity *identity);

/* The XUID and proof a secret gives: SHA-256("doa2u-live" || secret) is the
   proof, and the XUID is 0x0009 | the proof's first six bytes as a big-endian
   48-bit number, so the matchmaker can check one against the other. */
void recomp_live_identity_from_secret(const uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES],
    RecompLiveIdentity *identity);

#ifdef __cplusplus
}
#endif

#endif
