/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOA2U_RECOMP_LIVE_IDENTITY_H
#define DOA2U_RECOMP_LIVE_IDENTITY_H

/* The Xbox Live account this install logs on with: a 32-byte
   secret kept beside the netplay settings, the XUID the matchmaker knows it
   by, the proof that registers it and the key that logs it on. The same
   secret always gives the same account, so friends and stats survive a
   restart.
   The proof crosses the wire once, in the LOGON that registers the XUID with a
   matchmaker, and whoever reads that packet can make the key. Every later
   LOGON carries only an HMAC, keyed by the key, of a nonce the matchmaker
   issued for it, which is of no use to anyone afterwards. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_LIVE_IDENTITY_SECRET_BYTES = 32,
    RECOMP_LIVE_IDENTITY_PROOF_BYTES = 32,
    RECOMP_LIVE_IDENTITY_KEY_BYTES = 32,
    RECOMP_LIVE_HMAC_BYTES = 32,
    RECOMP_LIVE_HMAC_MESSAGE_BYTES = 192, /* the longest message recomp_live_hmac takes */
    /* The high 16 bits of an Xbox Live XUID; the low 48 come from SHA-256(proof). */
    RECOMP_LIVE_XUID_TYPE = 0x0009u,
    RECOMP_LIVE_IDENTITY_PATH_BYTES = 1024,
};

typedef struct RecompLiveIdentity {
    uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    uint64_t xuid;
    uint8_t proof[RECOMP_LIVE_IDENTITY_PROOF_BYTES];
    uint8_t key[RECOMP_LIVE_IDENTITY_KEY_BYTES];
    /* The file that lists the matchmakers this account has logged on to: the
       identity file's name and ".servers.txt". Empty for an identity made from
       a bare secret, which remembers none. */
    char servers[RECOMP_LIVE_IDENTITY_PATH_BYTES];
} RecompLiveIdentity;

/* Reads <storage>/live-identity.bin, or the file RECOMP_LIVE_IDENTITY names,
   and makes a random secret there on first use, which also removes a server
   list left beside it: a new account has logged on nowhere. False when the
   file is damaged, cannot be written, or no secret or digest can be made. */
bool recomp_live_identity_load(const char *storage, RecompLiveIdentity *identity);

/* The XUID, proof and key a secret gives: SHA-256("doa2u-live" || secret) is
   the proof (plain SHA-256, not HMAC), SHA-256(proof) is the key, which is
   what the matchmaker keeps of a proof, and the XUID is 0x0009 over the key's
   first six bytes as a big-endian 48-bit number, so the matchmaker can check a
   proof against its XUID.
   False leaves the identity empty when no digest could be made. */
bool recomp_live_identity_from_secret(const uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES],
    RecompLiveIdentity *identity);

/* HMAC-SHA256 (RFC 2104) of a short message under a 32-byte key. False when
   the message is longer than RECOMP_LIVE_HMAC_MESSAGE_BYTES or no digest could
   be made. */
bool recomp_live_hmac(const uint8_t key[RECOMP_LIVE_IDENTITY_KEY_BYTES], const uint8_t *message, size_t size,
    uint8_t mac[RECOMP_LIVE_HMAC_BYTES]);

/* The matchmakers this account has logged on to, one "host:port" a line in
   identity->servers. The proof registers the account with a matchmaker once,
   so one in this list is never sent it again (live_client.c). A missing list
   holds no server. One that is there but cannot be read may name any server,
   so it knows them all. */
bool recomp_live_identity_knows_server(const RecompLiveIdentity *identity, const char *server);
/* Adds a server to that list, replacing the file whole as the secret is
   written. False when the list could not be read or written; true when the server is
   in it, or when the identity keeps no list. */
bool recomp_live_identity_remember_server(const RecompLiveIdentity *identity, const char *server);

#ifdef __cplusplus
}
#endif

#endif
