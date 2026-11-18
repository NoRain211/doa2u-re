/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "live_identity.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* The runtime's SHA-256 (xbe/sha.cpp) as a raw digest; false when the host
   could not make one, which is reported here rather than thrown into C. */
bool doaxbv_sha256_digest(const uint8_t *bytes, size_t size, uint8_t digest[32]);

static const char identity_file[] = "live-identity.bin";
static const char servers_suffix[] = ".servers.txt";
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

/* Creates a new file only, readable by the player alone: the secret is the
   whole account, and a name already taken, by a file or by a symlink, belongs
   to somebody else and is never written through. */
static int create_exclusive(const char *path)
{
#ifdef _WIN32
    return _open(path, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
#endif
}

/* Writes a whole file under a temporary name and renames it into place, so a
   reader sees the old file or the new one. The secret is only ever made new;
   the server list is replaced, which a Windows rename does not do. */
static bool write_file(const char *path, const void *bytes, size_t size, bool replace)
{
    char temporary[RECOMP_LIVE_IDENTITY_PATH_BYTES + 8];
    const int needed = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (needed < 0 || (size_t)needed >= sizeof temporary) return false;
    int fd = create_exclusive(temporary);
    if (fd < 0 && errno == EEXIST) {
        /* A run killed between its create and its rename leaves this name
           behind. Removing it drops the name itself, so a planted symlink goes
           and its target stays, and the create is exclusive again: one
           interrupted save cannot keep Live unavailable for good. */
        remove(temporary);
        fd = create_exclusive(temporary);
    }
    if (fd < 0) return false;
#ifdef _WIN32
    FILE *file = _fdopen(fd, "wb");
#else
    FILE *file = fdopen(fd, "wb");
#endif
    if (file == NULL) {
#ifdef _WIN32
        _close(fd);
#else
        close(fd);
#endif
        return false;
    }
    const bool written = fwrite(bytes, 1, size, file) == size;
    const bool closed = fclose(file) == 0;
#ifdef _WIN32
    const bool moved = written && closed && (replace ?
        MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING) != 0 : rename(temporary, path) == 0);
#else
    (void)replace;
    const bool moved = written && closed && rename(temporary, path) == 0;
#endif
    if (!moved) remove(temporary);
    return moved;
}

bool recomp_live_identity_from_secret(const uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES],
    RecompLiveIdentity *identity)
{
    uint8_t material[sizeof proof_prefix - 1u + RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    identity->servers[0] = '\0';
    memcpy(identity->secret, secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    memcpy(material, proof_prefix, sizeof proof_prefix - 1u);
    memcpy(material + sizeof proof_prefix - 1u, secret, RECOMP_LIVE_IDENTITY_SECRET_BYTES);
    /* The XUID is derived from the proof the server checks, not from the
       secret, and only the first six bytes of SHA-256(proof) reach it, so the
       proof itself is not published: the type in the high 16 bits, then those
       bytes as a big-endian 48-bit number. A server can reject a LOGON whose
       XUID does not match its proof. That hash is also the key: the server
       keeps it from the proof that registered the XUID, and this side never
       sends it. */
    uint8_t *digest = identity->key;
    if (!doaxbv_sha256_digest(material, sizeof material, identity->proof) ||
        !doaxbv_sha256_digest(identity->proof, RECOMP_LIVE_IDENTITY_PROOF_BYTES, digest)) {
        /* Without a digest there is no account to publish: an empty identity
           makes the load fail instead of sending proof the server cannot
           check. */
        memset(identity, 0, sizeof *identity);
        return false;
    }
    identity->xuid = (uint64_t)RECOMP_LIVE_XUID_TYPE << 48 |
        (uint64_t)digest[0] << 40 | (uint64_t)digest[1] << 32 | (uint64_t)digest[2] << 24 |
        (uint64_t)digest[3] << 16 | (uint64_t)digest[4] << 8 | digest[5];
    return true;
}

bool recomp_live_hmac(const uint8_t key[RECOMP_LIVE_IDENTITY_KEY_BYTES], const uint8_t *message, size_t size,
    uint8_t mac[RECOMP_LIVE_HMAC_BYTES])
{
    enum { BLOCK = 64 }; /* SHA-256's; the key is shorter, so it is padded with zeros, not hashed */
    uint8_t inner[BLOCK + RECOMP_LIVE_HMAC_MESSAGE_BYTES], outer[BLOCK + 32];
    if (message == NULL || size > RECOMP_LIVE_HMAC_MESSAGE_BYTES) return false;
    for (unsigned i = 0u; i < BLOCK; ++i) {
        const uint8_t byte = i < RECOMP_LIVE_IDENTITY_KEY_BYTES ? key[i] : 0u;
        inner[i] = byte ^ 0x36u;
        outer[i] = byte ^ 0x5cu;
    }
    memcpy(inner + BLOCK, message, size);
    return doaxbv_sha256_digest(inner, BLOCK + size, outer + BLOCK) &&
        doaxbv_sha256_digest(outer, sizeof outer, mac);
}

bool recomp_live_identity_load(const char *storage, RecompLiveIdentity *identity)
{
    if (identity == NULL) return false;
    memset(identity, 0, sizeof *identity);
    char path[RECOMP_LIVE_IDENTITY_PATH_BYTES], servers[RECOMP_LIVE_IDENTITY_PATH_BYTES];
    const char *override = getenv("RECOMP_LIVE_IDENTITY");
    if (override != NULL && override[0] != '\0') {
        const int needed = snprintf(path, sizeof path, "%s", override);
        if (needed < 0 || (size_t)needed >= sizeof path) return false;
    } else {
        if (storage == NULL || storage[0] == '\0') return false;
        const int needed = snprintf(path, sizeof path, "%s/%s", storage, identity_file);
        if (needed < 0 || (size_t)needed >= sizeof path) return false;
    }
    const int listed = snprintf(servers, sizeof servers, "%s%s", path, servers_suffix);
    if (listed < 0 || (size_t)listed >= sizeof servers) return false;
    uint8_t secret[RECOMP_LIVE_IDENTITY_SECRET_BYTES];
    FILE *file = fopen(path, "rb");
    if (file != NULL) {
        const size_t got = fread(secret, 1, sizeof secret, file);
        const int extra = fgetc(file);
        fclose(file);
        if (got != sizeof secret || extra != EOF) return false; /* a damaged file is not replaced */
    } else if (errno != ENOENT || !random_secret(secret) || !write_file(path, secret, sizeof secret, false)) {
        /* Only a missing file is a first use: one that is there but cannot be
           opened (permissions, a sharing lock) keeps its account. */
        return false;
    } else {
        /* A list left by the account this one replaces would keep the new
           account from registering with those servers. */
        remove(servers);
    }
    if (!recomp_live_identity_from_secret(secret, identity)) return false;
    memcpy(identity->servers, servers, (size_t)listed + 1u);
    return identity->xuid != 0u;
}

bool recomp_live_identity_knows_server(const RecompLiveIdentity *identity, const char *server)
{
    char line[512];
    bool known = false;
    if (identity == NULL || server == NULL || server[0] == '\0' || identity->servers[0] == '\0') return false;
    FILE *file = fopen(identity->servers, "rb");
    if (file == NULL) return false;
    while (!known && fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        known = strcmp(line, server) == 0;
    }
    fclose(file);
    return known;
}

bool recomp_live_identity_remember_server(const RecompLiveIdentity *identity, const char *server)
{
    /* shortcut: a list over 4 KB (some 150 servers) takes no more, raise it when a player has that many */
    char text[4096];
    size_t size = 0u;
    if (identity == NULL || server == NULL || server[0] == '\0' || identity->servers[0] == '\0') return true;
    if (recomp_live_identity_knows_server(identity, server)) return true;
    FILE *file = fopen(identity->servers, "rb");
    if (file != NULL) {
        size = fread(text, 1, sizeof text - 1u, file);
        const bool failed = ferror(file) != 0;
        fclose(file);
        if (failed) return false;
    } else if (errno != ENOENT) {
        return false; /* there but unreadable: writing now would drop the servers it names */
    }
    if (size != 0u && text[size - 1u] != '\n') text[size++] = '\n'; /* a last line someone left open */
    const int needed = snprintf(text + size, sizeof text - size, "%s\n", server);
    if (needed < 0 || (size_t)needed >= sizeof text - size) return false;
    return write_file(identity->servers, text, size + (size_t)needed, true);
}
